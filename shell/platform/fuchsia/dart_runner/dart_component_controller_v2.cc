// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dart_component_controller_v2.h"

#include <fcntl.h>
#include <fml/logging.h>
#include <lib/async-loop/loop.h>
#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/namespace.h>
#include <lib/fidl/cpp/string.h>
#include <lib/sys/cpp/service_directory.h>
#include <lib/syslog/global.h>
#include <lib/zx/clock.h>
#include <lib/zx/thread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <zircon/status.h>

#include <regex>
#include <utility>

#include "runtime/dart/utils/files.h"
#include "runtime/dart/utils/handle_exception.h"
#include "runtime/dart/utils/inlines.h"
#include "runtime/dart/utils/tempfs.h"
#include "third_party/dart/runtime/include/dart_tools_api.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/dart_message_handler.h"
#include "third_party/tonic/dart_microtask_queue.h"
#include "third_party/tonic/dart_state.h"
#include "third_party/tonic/logging/dart_error.h"

#include "builtin_libraries.h"
#include "logging.h"

using tonic::ToDart;

namespace dart_runner {

namespace {

constexpr char kTmpPath[] = "/tmp";

constexpr zx::duration kIdleWaitDuration = zx::sec(2);
constexpr zx::duration kIdleNotifyDuration = zx::msec(500);
constexpr zx::duration kIdleSlack = zx::sec(1);

void AfterTask(async_loop_t*, void*) {
  tonic::DartMicrotaskQueue* queue =
      tonic::DartMicrotaskQueue::GetForCurrentThread();
  // Verify that the queue exists, as this method could have been called back as
  // part of the exit routine, after the destruction of the microtask queue.
  if (queue) {
    queue->RunMicrotasks();
  }
}

constexpr async_loop_config_t kLoopConfig = {
    .default_accessors =
        {
            .getter = async_get_default_dispatcher,
            .setter = async_set_default_dispatcher,
        },
    .make_default_for_current_thread = true,
    .epilogue = &AfterTask,
};

// Find the last path of the component.
// fuchsia-pkg://fuchsia.com/hello_dart#meta/hello_dart.cmx -> hello_dart.cmx
std::string GetLabelFromUrl(const std::string& url) {
  for (size_t i = url.length() - 1; i > 0; i--) {
    if (url[i] == '/') {
      return url.substr(i + 1, url.length() - 1);
    }
  }
  return url;
}

// Find the name of the component.
// fuchsia-pkg://fuchsia.com/hello_dart#meta/hello_dart.cm -> hello_dart
std::string GetComponentNameFromUrl(const std::string& url) {
  const std::string label = GetLabelFromUrl(url);
  for (size_t i = 0; i < label.length(); ++i) {
    if (label[i] == '.') {
      return label.substr(0, i);
    }
  }
  return label;
}

}  // namespace

DartComponentControllerV2::DartComponentControllerV2(
    fuchsia::component::runner::ComponentStartInfo start_info,
    std::shared_ptr<sys::ServiceDirectory> runner_incoming_services,
    fidl::InterfaceRequest<fuchsia::component::runner::ComponentController>
        controller)
    : loop_(new async::Loop(&kLoopConfig)),
      label_(GetLabelFromUrl(start_info.resolved_url())),
      url_(std::move(start_info.resolved_url())),
      runner_incoming_services_(runner_incoming_services),
      start_info_(std::move(start_info)),
      binding_(this) {
  // TODO(fxb/84537): This data path is configured based how we build Flutter
  // applications in tree currently, but the way we build the Flutter
  // application may change. We should avoid assuming the data path and let the
  // CML file specify this data path instead.
  const std::string component_name = GetComponentNameFromUrl(url_);
  data_path_ = "pkg/data/" + component_name;

  if (controller.is_valid()) {
    binding_.Bind(std::move(controller));
    binding_.set_error_handler([this](zx_status_t status) { Kill(); });
  } else {
    FX_LOG(ERROR, LOG_TAG,
           "Fuchsia component controller endpoint is not valid.");
  }

  zx_status_t idle_timer_status =
      zx::timer::create(ZX_TIMER_SLACK_LATE, ZX_CLOCK_MONOTONIC, &idle_timer_);
  if (idle_timer_status != ZX_OK) {
    FX_LOGF(INFO, LOG_TAG, "Idle timer creation failed: %s",
            zx_status_get_string(idle_timer_status));
  } else {
    idle_wait_.set_object(idle_timer_.get());
    idle_wait_.set_trigger(ZX_TIMER_SIGNALED);
    idle_wait_.Begin(async_get_default_dispatcher());
  }
}

DartComponentControllerV2::~DartComponentControllerV2() {
  if (namespace_) {
    fdio_ns_destroy(namespace_);
    namespace_ = nullptr;
  }
  close(stdout_fd_);
  close(stderr_fd_);
}

bool DartComponentControllerV2::SetUp() {
  // Name the thread after the url of the component being launched.
  zx::thread::self()->set_property(ZX_PROP_NAME, label_.c_str(), label_.size());
  Dart_SetThreadName(label_.c_str());

  if (!CreateAndBindNamespace()) {
    return false;
  }

  if (SetUpFromAppSnapshot()) {
    FX_LOGF(INFO, LOG_TAG, "%s is running from an app snapshot", url_.c_str());
  } else if (SetUpFromKernel()) {
    FX_LOGF(INFO, LOG_TAG, "%s is running from kernel", url_.c_str());
  } else {
    FX_LOGF(ERROR, LOG_TAG, "Failed to set up component controller for %s.",
            url_.c_str());
    return false;
  }

  return true;
}

bool DartComponentControllerV2::CreateAndBindNamespace() {
  if (!start_info_.has_ns()) {
    FX_LOG(ERROR, LOG_TAG, "Component start info does not have a namespace.");
    return false;
  }

  const zx_status_t ns_create_status = fdio_ns_create(&namespace_);
  if (ns_create_status != ZX_OK) {
    FX_LOGF(ERROR, LOG_TAG, "Failed to create namespace: %s",
            zx_status_get_string(ns_create_status));
  }

  dart_utils::RunnerTemp::SetupComponent(namespace_);

  // Bind each directory in start_info's namespace to the controller's namespace
  // instance.
  for (auto& ns_entry : *start_info_.mutable_ns()) {
    // TODO(akbiggs): Under what circumstances does a namespace entry not have a
    // path or directory? Should we log an error for these?
    if (!ns_entry.has_path() || !ns_entry.has_directory()) {
      continue;
    }

    if (ns_entry.path() == kTmpPath) {
      // /tmp is covered by the local memfs.
      continue;
    }

    // We move ownership of the directory & path since RAII is used to keep
    // the handle open.
    fidl::InterfaceHandle<::fuchsia::io::Directory> dir =
        std::move(*ns_entry.mutable_directory());
    const std::string path = std::move(*ns_entry.mutable_path());

    const zx_status_t ns_bind_status =
        fdio_ns_bind(namespace_, path.c_str(), dir.TakeChannel().release());
    if (ns_bind_status != ZX_OK) {
      FX_LOGF(ERROR, LOG_TAG, "Failed to bind %s to namespace: %s",
              path.c_str(), zx_status_get_string(ns_bind_status));
      return false;
    }
  }

  return true;
}

bool DartComponentControllerV2::SetUpFromKernel() {
  dart_utils::MappedResource manifest;
  if (!dart_utils::MappedResource::LoadFromNamespace(
          namespace_, data_path_ + "/app.dilplist", manifest)) {
    return false;
  }

  if (!dart_utils::MappedResource::LoadFromNamespace(
          nullptr, "/pkg/data/isolate_core_snapshot_data.bin",
          isolate_snapshot_data_)) {
    return false;
  }
  if (!dart_utils::MappedResource::LoadFromNamespace(
          nullptr, "/pkg/data/isolate_core_snapshot_instructions.bin",
          isolate_snapshot_instructions_, true /* executable */)) {
    return false;
  }

  if (!CreateIsolate(isolate_snapshot_data_.address(),
                     isolate_snapshot_instructions_.address())) {
    return false;
  }

  Dart_EnterScope();

  std::string str(reinterpret_cast<const char*>(manifest.address()),
                  manifest.size());
  Dart_Handle library = Dart_Null();
  for (size_t start = 0; start < manifest.size();) {
    size_t end = str.find("\n", start);
    if (end == std::string::npos) {
      FX_LOG(ERROR, LOG_TAG, "Malformed manifest");
      Dart_ExitScope();
      return false;
    }

    std::string path = data_path_ + "/" + str.substr(start, end - start);
    start = end + 1;

    dart_utils::MappedResource kernel;
    if (!dart_utils::MappedResource::LoadFromNamespace(namespace_, path,
                                                       kernel)) {
      FX_LOGF(ERROR, LOG_TAG, "Cannot load kernel from namespace: %s",
              path.c_str());
      Dart_ExitScope();
      return false;
    }
    library = Dart_LoadLibraryFromKernel(kernel.address(), kernel.size());
    if (Dart_IsError(library)) {
      FX_LOGF(ERROR, LOG_TAG, "Cannot load library from kernel: %s",
              Dart_GetError(library));
      Dart_ExitScope();
      return false;
    }

    kernel_peices_.emplace_back(std::move(kernel));
  }
  Dart_SetRootLibrary(library);

  Dart_Handle result = Dart_FinalizeLoading(false);
  if (Dart_IsError(result)) {
    FX_LOGF(ERROR, LOG_TAG, "Failed to FinalizeLoading: %s",
            Dart_GetError(result));
    Dart_ExitScope();
    return false;
  }

  return true;
}

bool DartComponentControllerV2::SetUpFromAppSnapshot() {
#if !defined(AOT_RUNTIME)
  return false;
#else
  // Load the ELF snapshot as available, and fall back to a blobs snapshot
  // otherwise.
  const uint8_t *isolate_data, *isolate_instructions;
  if (elf_snapshot_.Load(namespace_, data_path_ + "/app_aot_snapshot.so")) {
    isolate_data = elf_snapshot_.IsolateData();
    isolate_instructions = elf_snapshot_.IsolateInstrs();
    if (isolate_data == nullptr || isolate_instructions == nullptr) {
      return false;
    }
  } else {
    if (!dart_utils::MappedResource::LoadFromNamespace(
            namespace_, data_path_ + "/isolate_snapshot_data.bin",
            isolate_snapshot_data_)) {
      return false;
    }
    if (!dart_utils::MappedResource::LoadFromNamespace(
            namespace_, data_path_ + "/isolate_snapshot_instructions.bin",
            isolate_snapshot_instructions_, true /* executable */)) {
      return false;
    }
  }
  return CreateIsolate(isolate_data, isolate_instructions);
#endif  // defined(AOT_RUNTIME)
}

bool DartComponentControllerV2::CreateIsolate(
    const uint8_t* isolate_snapshot_data,
    const uint8_t* isolate_snapshot_instructions) {
  // Create the isolate from the snapshot.
  char* error = nullptr;

  // TODO(dart_runner): Pass if we start using tonic's loader.
  intptr_t namespace_fd = -1;

  // Freed in IsolateShutdownCallback.
  auto state = new std::shared_ptr<tonic::DartState>(new tonic::DartState(
      namespace_fd, [this](Dart_Handle result) { MessageEpilogue(result); }));

  isolate_ = Dart_CreateIsolateGroup(
      url_.c_str(), label_.c_str(), isolate_snapshot_data,
      isolate_snapshot_instructions, nullptr /* flags */, state, state, &error);
  if (!isolate_) {
    FX_LOGF(ERROR, LOG_TAG, "Dart_CreateIsolateGroup failed: %s", error);
    return false;
  }

  state->get()->SetIsolate(isolate_);

  tonic::DartMessageHandler::TaskDispatcher dispatcher =
      [loop = loop_.get()](auto callback) {
        async::PostTask(loop->dispatcher(), std::move(callback));
      };
  state->get()->message_handler().Initialize(dispatcher);

  state->get()->SetReturnCodeCallback(
      [this](uint32_t return_code) { return_code_ = return_code; });

  return true;
}

void DartComponentControllerV2::Run() {
  async::PostTask(loop_->dispatcher(), [loop = loop_.get(), app = this] {
    if (!app->RunDartMain()) {
      loop->Quit();
    }
  });
  loop_->Run();

  if (binding_.is_bound()) {
    // From the documentation for ComponentController, ZX_OK should be sent when
    // the ComponentController receives a termination request. However, if the
    // component exited with a non-zero return code, we indicate this by sending
    // an INTERNAL epitaph instead.
    //
    // TODO(fxb/86666): Communicate return code from the ComponentController
    // once v2 has support.
    if (return_code_ == 0) {
      binding_.Close(ZX_OK);
    } else {
      FML_LOG(ERROR) << "Component exited with non-zero return code: "
                     << return_code_;
      binding_.Close(zx_status_t(fuchsia::component::Error::INTERNAL));
    }
  }
}

bool DartComponentControllerV2::RunDartMain() {
  FML_CHECK(namespace_ != nullptr);
  Dart_EnterScope();

  tonic::DartMicrotaskQueue::StartForCurrentThread();

  // TODO(fxb/79871): Create a file descriptor for each component that is
  // launched and listen for anything that is written to the component. When
  // something is written to the component, forward that message along to the
  // Fuchsia logger and decorate it with the tag that it came from the
  // component.
  stdout_fd_ = fileno(stdout);
  stderr_fd_ = fileno(stderr);

  fidl::InterfaceRequest<fuchsia::io::Directory> outgoing_dir =
      std::move(*start_info_.mutable_outgoing_dir());
  InitBuiltinLibrariesForIsolate(
      url_, namespace_, stdout_fd_, stderr_fd_, nullptr /* environment */,
      outgoing_dir.TakeChannel(), false /* service_isolate */);

  Dart_ExitScope();
  Dart_ExitIsolate();
  char* error = Dart_IsolateMakeRunnable(isolate_);
  if (error != nullptr) {
    Dart_EnterIsolate(isolate_);
    Dart_ShutdownIsolate();
    FX_LOGF(ERROR, LOG_TAG, "Unable to make isolate runnable: %s", error);
    free(error);
    return false;
  }
  Dart_EnterIsolate(isolate_);
  Dart_EnterScope();

  // TODO(fxb/79871): Support argument passing.
  // Note: Even though we do not support argument passing via the cml files
  // at this time, we still need to create an argument list and pass it off
  // to the invocation of main below. If we do not do this dart will look for
  // a function with the signature `void main()` but existing dart components
  // that run in the dart runner are written with main functions that have the
  // signature `void main(List<String> args)`. In order to ensure that these
  // components do not break we need to have this stub argument list.
  Dart_Handle dart_arguments = Dart_NewListOf(Dart_CoreType_String, 0);

  if (Dart_IsError(dart_arguments)) {
    FX_LOGF(ERROR, LOG_TAG, "Failed to allocate Dart arguments list: %s",
            Dart_GetError(dart_arguments));
    Dart_ExitScope();
    return false;
  }

  Dart_Handle argv[] = {
      dart_arguments,
  };

  Dart_Handle main_result =
      Dart_Invoke(Dart_RootLibrary() /* target */, ToDart("main") /* name */,
                  dart_utils::ArraySize(argv) /* number_of_arguments */,
                  argv /* arguments */);

  if (Dart_IsError(main_result)) {
    auto dart_state = tonic::DartState::Current();
    if (!dart_state->has_set_return_code()) {
      // The program hasn't set a return code meaning this exit is unexpected.
      FX_LOG(ERROR, LOG_TAG, Dart_GetError(main_result));
      return_code_ = tonic::GetErrorExitCode(main_result);

      dart_utils::HandleIfException(runner_incoming_services_, url_,
                                    main_result);
    }
    Dart_ExitScope();
    return false;
  }

  Dart_ExitScope();
  return true;
}

void DartComponentControllerV2::Kill() {
  if (Dart_CurrentIsolate()) {
    tonic::DartMicrotaskQueue* queue =
        tonic::DartMicrotaskQueue::GetForCurrentThread();
    if (queue) {
      queue->Destroy();
    }

    loop_->Quit();

    // TODO(rosswang): The docs warn of threading issues if doing this again,
    // but without this, attempting to shut down the isolate finalizes app
    // contexts that can't tell a shutdown is in progress and so fatal.
    Dart_SetMessageNotifyCallback(nullptr);

    Dart_ShutdownIsolate();
  }
}

void DartComponentControllerV2::MessageEpilogue(Dart_Handle result) {
  auto dart_state = tonic::DartState::Current();
  // If the Dart program has set a return code, then it is intending to shut
  // down by way of a fatal error, and so there is no need to override
  // return_code_.
  if (dart_state->has_set_return_code()) {
    Dart_ShutdownIsolate();
    return;
  }

  dart_utils::HandleIfException(runner_incoming_services_, url_, result);

  // Otherwise, see if there was any other error.
  return_code_ = tonic::GetErrorExitCode(result);
  if (return_code_ != 0) {
    Dart_ShutdownIsolate();
    return;
  }

  idle_start_ = zx::clock::get_monotonic();
  zx_status_t status =
      idle_timer_.set(idle_start_ + kIdleWaitDuration, kIdleSlack);
  if (status != ZX_OK) {
    FX_LOGF(INFO, LOG_TAG, "Idle timer set failed: %s",
            zx_status_get_string(status));
  }
}

void DartComponentControllerV2::Stop() {
  Kill();
}

void DartComponentControllerV2::OnIdleTimer(async_dispatcher_t* dispatcher,
                                            async::WaitBase* wait,
                                            zx_status_t status,
                                            const zx_packet_signal* signal) {
  if ((status != ZX_OK) || !(signal->observed & ZX_TIMER_SIGNALED) ||
      !Dart_CurrentIsolate()) {
    // Timer closed or isolate shutdown.
    return;
  }

  zx::time deadline = idle_start_ + kIdleWaitDuration;
  zx::time now = zx::clock::get_monotonic();
  if (now >= deadline) {
    // No Dart message has been processed for kIdleWaitDuration: assume we'll
    // stay idle for kIdleNotifyDuration.
    Dart_NotifyIdle((now + kIdleNotifyDuration).get());
    idle_start_ = zx::time(0);
    idle_timer_.cancel();  // De-assert signal.
  } else {
    // Early wakeup or message pushed idle time forward: reschedule.
    zx_status_t status = idle_timer_.set(deadline, kIdleSlack);
    if (status != ZX_OK) {
      FX_LOGF(INFO, LOG_TAG, "Idle timer set failed: %s",
              zx_status_get_string(status));
    }
  }
  wait->Begin(dispatcher);  // ignore errors
}

}  // namespace dart_runner
