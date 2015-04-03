// Copyright 2007-2010 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
// ========================================================================
//
// For interactive instances, do not access the network before displaying the
// UI. This provides a better user experience - quick UI - when the network is
// slow. It is also required to ensure that UI displayed event is signaled
// before potentially waiting on a firewall prompt.
//
// Debugging notes:
//  * Omaha initial install:
//      /install "appguid={8A69D345-D564-463C-AFF1-A69D9E530F96}&appname=Google%20Chrome&needsadmin=False&lang=en"  // NOLINT
//      /install "appguid={283EAF47-8817-4c2b-A801-AD1FADFB7BAA}&appname=Gears&needsadmin=True&lang=en"  // NOLINT
//  * App install:
//      /handoff "appguid={8A69D345-D564-463C-AFF1-A69D9E530F96}&appname=Google%20Chrome&needsadmin=False&lang=en"  // NOLINT
//      /handoff "appguid={283EAF47-8817-4c2b-A801-AD1FADFB7BAA}&appname=Gears&needsadmin=True&lang=en"  // NOLINT
//  * Silent install:
//   * Add "/silent" to any of the above command lines (not to the tag).
//  * ViaSat Update self-update:
//      /update
//  * Update check for apps that need it:
//      /ua
//  * Core:
//      /c
//  * Cod Red check:
//      /cr
//  * Cod Red repair:
//      /recover [/machine]
//  * OneClick:
//      /pi "http://www.google.com/" "/install%20%22appguid=%7B8A69D345-D564-463C-AFF1-A69D9E530F96%7D%26lang=en%26appname=Google%2520Chrome%26needsadmin=false" /installsource oneclick  // NOLINT
//  * COM server:
//      -Embedding

#include "omaha/goopdate/goopdate.h"

#include <atlstr.h>
#include <new>
#include "base/scoped_ptr.h"
#include "omaha/base/app_util.h"
#include "omaha/base/const_object_names.h"
#include "omaha/base/crash_if_specific_error.h"
#include "omaha/base/debug.h"
#include "omaha/base/error.h"
#include "omaha/base/file.h"
#include "omaha/base/logging.h"
#include "omaha/base/module_utils.h"
#include "omaha/base/omaha_version.h"
#include "omaha/base/proc_utils.h"
#include "omaha/base/reg_key.h"
#include "omaha/base/safe_format.h"
#include "omaha/base/scoped_any.h"
#include "omaha/base/scoped_ptr_address.h"
#include "omaha/base/system_info.h"
#include "omaha/base/utils.h"
#include "omaha/base/vistautil.h"
#include "omaha/client/client_utils.h"
#include "omaha/client/install.h"
#include "omaha/client/install_apps.h"
#include "omaha/client/install_self.h"
#include "omaha/client/resource.h"  // IDS_* are used in client modes only.
#include "omaha/client/ua.h"
#include "omaha/common/app_registry_utils.h"
#include "omaha/common/command_line.h"
#include "omaha/common/config_manager.h"
#include "omaha/common/const_cmd_line.h"
#include "omaha/common/const_goopdate.h"
#include "omaha/common/goopdate_utils.h"
#include "omaha/common/ping.h"
#include "omaha/common/lang.h"
#include "omaha/common/oem_install_utils.h"
#include "omaha/common/stats_uploader.h"
#include "omaha/common/webplugin_utils.h"
#include "omaha/core/core.h"
#include "omaha/core/crash_handler.h"
#include "omaha/goopdate/code_red_check.h"
#include "omaha/goopdate/crash.h"
#include "omaha/goopdate/google_update.h"
#include "omaha/goopdate/goopdate_internal.h"
#include "omaha/goopdate/goopdate_metrics.h"
#include "omaha/goopdate/resource_manager.h"
#include "omaha/net/net_diags.h"
#include "omaha/service/service_main.h"
#include "omaha/setup/setup_service.h"
#include "third_party/breakpad/src/client/windows/sender/crash_report_sender.h"
#include "third_party/breakpad/src/client/windows/handler/exception_handler.h"

// TODO(omaha3): Where should we put this? In Omaha 2, it was in worker.cc.
// TODO(omaha): fix this clunkiness and require explicit registration of the
// http creators with the factory. Not ideal but better then linker options.
#pragma comment(linker, "/INCLUDE:_kRegisterWinHttp")

namespace omaha {

namespace {

#if DEBUG
const TCHAR* const kBuildType = _T("dbg");
#else
const TCHAR* const kBuildType = _T("opt");
#endif

#if OFFICIAL_BUILD
const TCHAR* const kOfficialBuild = _T("official");
#else
const TCHAR* const kOfficialBuild = _T("dev");
#endif

#if DEBUG
// Returns true if the binary's version matches the installed version or this
// mode does not require the versions to match.
bool CheckRegisteredVersion(const CString& version,
                            bool is_machine,
                            CommandLineMode mode) {
  switch (mode) {
    // Modes that may run before or during installation or otherwise do not
    // need to match the installed version.
    case COMMANDLINE_MODE_UNKNOWN:
    case COMMANDLINE_MODE_NOARGS:
    case COMMANDLINE_MODE_REGSERVER:
    case COMMANDLINE_MODE_UNREGSERVER:
    case COMMANDLINE_MODE_NETDIAGS:
    case COMMANDLINE_MODE_CRASH:
    case COMMANDLINE_MODE_REPORTCRASH:
    case COMMANDLINE_MODE_INSTALL:
    case COMMANDLINE_MODE_UPDATE:
    case COMMANDLINE_MODE_RECOVER:
    case COMMANDLINE_MODE_WEBPLUGIN:
    case COMMANDLINE_MODE_REGISTER_PRODUCT:
    case COMMANDLINE_MODE_UNREGISTER_PRODUCT:
    case COMMANDLINE_MODE_SERVICE_REGISTER:
    case COMMANDLINE_MODE_SERVICE_UNREGISTER:
    case COMMANDLINE_MODE_PING:
      return true;

    // COM servers and services that should only run after installation.
    case COMMANDLINE_MODE_CORE:
    case COMMANDLINE_MODE_SERVICE:
    case COMMANDLINE_MODE_CODE_RED_CHECK:
    case COMMANDLINE_MODE_COMSERVER:
    case COMMANDLINE_MODE_CRASH_HANDLER:
    case COMMANDLINE_MODE_COMBROKER:
    case COMMANDLINE_MODE_ONDEMAND:
    case COMMANDLINE_MODE_MEDIUM_SERVICE:

    // Clients that should only run after installation and should match the
    // installed version.
    case COMMANDLINE_MODE_HANDOFF_INSTALL:
    case COMMANDLINE_MODE_UA:
    case COMMANDLINE_MODE_UNINSTALL:

    default:
      // This binary's version should be the installed version.
      CString installed_version;
      VERIFY1(SUCCEEDED(RegKey::GetValue(
          ConfigManager::Instance()->registry_update(is_machine),
          kRegValueInstalledVersion,
          &installed_version)));
      return version == installed_version;
  }
}
#endif

}  // namespace

namespace detail {

class GoopdateImpl {
 public:
  GoopdateImpl(Goopdate* goopdate, bool is_local_system);
  ~GoopdateImpl();

  HRESULT Main(HINSTANCE instance, const TCHAR* cmd_line, int cmd_show);

  HRESULT QueueUserWorkItem(UserWorkItem* work_item, uint32 flags);

  void Stop();

  bool is_local_system() const { return is_local_system_; }

 private:
  HRESULT DoMain(HINSTANCE instance, const TCHAR* cmd_line, int cmd_show);

  // Performs initialization that must be done as soon as the command line has
  // been parsed and loads the resources. If this method succeeds, we can use
  // the resources - for example, to display error messagse.
  HRESULT InitializeGoopdateAndLoadResources();

  // Executes the mode determined by DoMain().
  HRESULT ExecuteMode(bool* has_ui_been_displayed);

  // Returns whether a process is a machine process.
  // Does not determine whether the process has the appropriate privileges.
  bool IsMachineProcess();

  bool ShouldCheckShutdownEvent(CommandLineMode mode);
  bool IsShutdownEventSet();

  HRESULT LoadResourceDllIfNecessary(CommandLineMode mode,
                                     const CString& resource_dir);

  HRESULT SetUsageStatsEnable();

  // Handles error conditions by showing UI if appropriate.
  void HandleError(HRESULT hr, bool has_ui_been_displayed);

  // Handles response to /pi command.
  HRESULT HandleWebPlugin();

  // Handles responses to /cr command.
  HRESULT HandleCodeRedCheck();

  // Handles /report command.
  HRESULT HandleReportCrash();

  // Installs apps for the /handoff instance.
  HRESULT DoHandoff(bool* has_ui_been_displayed);

  // Updates all registered apps for the /ua instance.
  HRESULT DoUpdateAllApps(bool* has_ui_been_displayed);

  // Generates a divide by zero to trigger breakpad dump.
  // Is only enabled in debug builds.
  HRESULT DoCrash();

  // Install Omaha.
  HRESULT DoInstall(bool* has_ui_been_displayed);

  // Silently update Omaha.
  HRESULT DoSelfUpdate();

  // Handles the recover command in ViaSat Update.
  HRESULT DoRecover();

  // Uninstalls Omaha if appropriate.
  HRESULT HandleUninstall();

  // Pings the Omaha server with a string.
  HRESULT HandlePing();

  // TODO(omaha): Reconcile the two uninstall functions and paths.
  void MaybeUninstallGoogleUpdate();

  // Uninstalls ViaSat Update if a /install process failed to install itself
  // or the app and there are no other apps registered.
  HRESULT UninstallIfNecessary();

  // Called by operator new or operator new[] when they cannot satisfy
  // a request for additional storage.
  static void OutOfMemoryHandler();

  static HRESULT CaptureOSMetrics();

  HINSTANCE module_instance_;  // Current module instance.
  CString cmd_line_;           // Command line, as provided by the OS.
  int cmd_show_;

  CommandLineArgs args_;       // Command line options and flags.

  // True if the process belongs to a machine Omaha "session".
  bool is_machine_;
  bool is_local_system_;       // True if running as LOCAL_SYSTEM.
  CString this_version_;       // Version of this Goopdate DLL.

  // True if Omaha has been uninstalled by the Worker.
  bool has_uninstalled_;

  // Language identifier for the current user locale.
  CString user_default_language_id_;

  scoped_ptr<ThreadPool>      thread_pool_;

  Goopdate* goopdate_;

  DISALLOW_EVIL_CONSTRUCTORS(GoopdateImpl);
};

GoopdateImpl::GoopdateImpl(Goopdate* goopdate, bool is_local_system)
    : module_instance_(NULL),
      cmd_show_(0),
      is_local_system_(is_local_system),
      has_uninstalled_(false),
      goopdate_(goopdate) {
  ASSERT1(goopdate);

  ++metric_goopdate_constructor;

  // The command line needs to be parsed to accurately determine if the current
  // process is a machine process or not. Take an upfront guess before that.
  is_machine_ = vista_util::IsUserAdmin() &&
      goopdate_utils::IsRunningFromOfficialGoopdateDir(true);

  // Install an error-handling mechanism which gets called when new operator
  // fails to allocate memory.
  VERIFY1(set_new_handler(&GoopdateImpl::OutOfMemoryHandler) == 0);

  // Install the exception handler.
  VERIFY1(SUCCEEDED(Crash::InstallCrashHandler(is_machine_)));

  // Hints network configure manager how to create its singleton.
  NetworkConfigManager::set_is_machine(is_machine_);

  // Initialize the global metrics collection.
  stats_report::g_global_metrics.Initialize();

  // TODO(omaha): Support multiple HRESULT codes to crash on.
  // TODO(omaha): Support passing in HRESULT codes via the command line,
  // especially for "/update".
  DWORD crash_specific_error = 0;
  if (SUCCEEDED(RegKey::GetValue(MACHINE_REG_UPDATE_DEV,
                                 kRegValueNameCrashIfSpecificError,
                                 &crash_specific_error))) {
    omaha::g_crash_specific_error = static_cast<HRESULT>(crash_specific_error);
  }

  static const int kThreadPoolShutdownDelayMs = 60000;
  thread_pool_.reset(new ThreadPool);
  HRESULT hr = thread_pool_->Initialize(kThreadPoolShutdownDelayMs);
  if (FAILED(hr)) {
    CORE_LOG(LE, (_T("[thread_pool_->Initialize failed][0x%08x]"), hr));
  }
}

GoopdateImpl::~GoopdateImpl() {
  CORE_LOG(L2, (_T("[GoopdateImpl::~GoopdateImpl]")));

  ++metric_goopdate_destructor;

  Stop();

  // The global metrics collection must be uninitialized before the metrics
  // destructors are called.
  stats_report::g_global_metrics.Uninitialize();

  // Uninstall the exception handler. Program crashes are handled by Windows
  // Error Reporting (WER) beyond this point.
  Crash::UninstallCrashHandler();

  // Reset the new handler.
  set_new_handler(NULL);
}

HRESULT GoopdateImpl::QueueUserWorkItem(UserWorkItem* work_item, uint32 flags) {
  CORE_LOG(L3, (_T("[GoopdateImpl::QueueUserWorkItem]")));
  ASSERT1(work_item);

  ASSERT1(thread_pool_.get());

  return thread_pool_->QueueUserWorkItem(work_item, flags);
}

void GoopdateImpl::Stop() {
  // The thread pool destructor waits for any remaining jobs to complete.
  thread_pool_.reset();
}

// Assumes the resources are loaded and members are initialized.
void GoopdateImpl::HandleError(HRESULT hr, bool has_ui_been_displayed) {
  CORE_LOG(L3, (_T("[GoopdateImpl::HandleError][0x%x][%u]"),
      hr, has_ui_been_displayed));

  if (has_ui_been_displayed ||
      !internal::CanDisplayUi(args_.mode, args_.is_silent_set)) {
    return;
  }

  const CString& bundle_name = args_.extra.bundle_name;
  CString primary_app_guid;
  if (!args_.extra.apps.empty()) {
    primary_app_guid = GuidToString(args_.extra.apps[0].app_guid);
  }

  CString error_text;
  switch (hr) {
    case GOOPDATE_E_UA_ALREADY_RUNNING:
      error_text.FormatMessage(IDS_APPLICATION_ALREADY_INSTALLING,
                               client_utils::GetUpdateAllAppsBundleName());
      break;
    case OMAHA_NET_E_WINHTTP_NOT_AVAILABLE:
      ASSERT1(!bundle_name.IsEmpty());
      error_text.FormatMessage(IDS_WINDOWS_IS_NOT_UP_TO_DATE,
                               bundle_name);
      break;
    default:
      // TODO(omaha3): This currently assumes that any error returned here is
      // related to Setup.
      CString product_name;
      VERIFY1(product_name.LoadString(IDS_PRODUCT_DISPLAY_NAME));
      error_text.FormatMessage(IDS_SETUP_FAILED, product_name, hr);
      break;
  }

  VERIFY1(client_utils::DisplayError(is_machine_,
               bundle_name,
               hr,
               0,
               error_text,
               primary_app_guid,
               args_.extra.language,
               args_.extra.installation_id,
               args_.extra.brand_code));
}

HRESULT GoopdateImpl::Main(HINSTANCE instance,
                           const TCHAR* cmd_line,
                           int cmd_show) {
  ++metric_goopdate_main;

  HRESULT hr = DoMain(instance, cmd_line, cmd_show);

  CORE_LOG(L2, (_T("[has_uninstalled_ is %d]"), has_uninstalled_));

  // For install processes, verify the ViaSat Update EULA has been accepted and
  // we can use the network unless a) the command line specifies EULA is
  // required or b) in OEM installing mode, which also prevents network use.
  if ((COMMANDLINE_MODE_INSTALL == args_.mode ||
       COMMANDLINE_MODE_HANDOFF_INSTALL == args_.mode) &&
       SUCCEEDED(hr)) {
    ASSERT1(args_.is_eula_required_set ||
            ConfigManager::Instance()->CanUseNetwork(is_machine_) ||
            oem_install_utils::IsOemInstalling(is_machine_));
  }

  // In the /install case, clean up if ViaSat Update and/or app install did not
  // complete successfully.
  // Only aggregate the metrics if there is no chance that ViaSat Update has
  // or may be uninstalled and the process has the appropriate permissions.
  // Uninstall will aggregate and report the metrics as appropriate.
  bool did_install_uninstall_fail = false;
  if (COMMANDLINE_MODE_INSTALL == args_.mode) {
    did_install_uninstall_fail = FAILED(UninstallIfNecessary());
  } else if (!has_uninstalled_) {
    if (args_.mode == COMMANDLINE_MODE_UA) {
      VERIFY1(SUCCEEDED(AggregateAndReportMetrics(is_machine_, false)));
    } else if (!is_machine_ || vista_util::IsUserAdmin()) {
      VERIFY1(SUCCEEDED(AggregateMetrics(is_machine_)));
    }
  }

  Worker::DeleteInstance();

  // Uninitializing the network configuration must happen after reporting the
  // metrics. The call succeeds even if the network has not been initialized
  // due to errors up the execution path.
  NetworkConfigManager::DeleteInstance();

  if (COMMANDLINE_MODE_INSTALL == args_.mode &&
      args_.is_oem_set &&
      SUCCEEDED(hr) &&
      !oem_install_utils::IsOemInstalling(is_machine_)) {
    ASSERT1(false);
    hr = GOOPDATE_E_OEM_INSTALL_SUCCEEDED_BUT_NOT_IN_OEM_INSTALLING_MODE;
  }

  // Verify that ViaSat Update is either completely installed or uninstalled.
  // Do not check in the following cases:
  // * Modes that may exit during Setup, during uninstall, or while Omaha
  //   is partially installed.
  // * The mode is unknown, which means the args were not be parsed.
  // * /cr instance, which may exit after Omaha is uninstalled.
  // * /install instance that would not have called Setup.Uninstall().
  // * /install instance when Uninstall failed for some reason since the
  //   the consistency check may expect the wrong state.
  // * /update instance that failed due to an install/uninstall in progress.
  if (COMMANDLINE_MODE_REGSERVER != args_.mode &&
      COMMANDLINE_MODE_UNREGSERVER != args_.mode &&
      COMMANDLINE_MODE_COMSERVER != args_.mode &&
      COMMANDLINE_MODE_CODE_RED_CHECK != args_.mode &&
      COMMANDLINE_MODE_SERVICE_REGISTER != args_.mode &&
      COMMANDLINE_MODE_SERVICE_UNREGISTER != args_.mode &&
      COMMANDLINE_MODE_UNKNOWN != args_.mode &&
      !(COMMANDLINE_MODE_INSTALL == args_.mode &&
        is_machine_ &&
        !vista_util::IsUserAdmin()) &&
      !(COMMANDLINE_MODE_UPDATE == args_.mode &&
        GOOPDATE_E_FAILED_TO_GET_LOCK == hr) &&
      !did_install_uninstall_fail) {
    install_self::CheckInstallStateConsistency(is_machine_);
  }

  ResourceManager::Delete();

  return hr;
}

HRESULT GoopdateImpl::DoMain(HINSTANCE instance,
                             const TCHAR* cmd_line,
                             int cmd_show) {
  module_instance_ = instance;
  cmd_line_ = cmd_line;
  cmd_show_ = cmd_show;

  // The system terminates the process without displaying a retry dialog box
  // for the user. GoogleUpdate has no user state to be saved, therefore
  // prompting the user for input is meaningless.
  VERIFY1(SUCCEEDED(SetProcessSilentShutdown()));

  VERIFY1(SUCCEEDED(CaptureOSMetrics()));

  InitializeVersionFromModule(module_instance_);
  this_version_ = GetVersionString();

  TCHAR path[MAX_PATH] = {0};
  VERIFY1(::GetModuleFileName(module_instance_, path, MAX_PATH));
  OPT_LOG(L1, (_T("[%s][version %s][%s][%s]"),
               path, this_version_, kBuildType, kOfficialBuild));

  CORE_LOG(L2, (_T("[is system %d]")
                _T("[elevated admin %d]")
                _T("[non-elevated admin %d]")
                _T("[testsource %s]"),
                is_local_system_,
                vista_util::IsUserAdmin(),
                vista_util::IsUserNonElevatedAdmin(),
                ConfigManager::Instance()->GetTestSource()));

  HRESULT parse_hr = omaha::ParseCommandLine(cmd_line_, &args_);
  if (FAILED(parse_hr)) {
    CORE_LOG(LE, (_T("[Parse cmd line failed][0x%08x]"), parse_hr));
    args_.mode = COMMANDLINE_MODE_UNKNOWN;
    // Continue because we want to load the resources and display an error.
  }

  // TODO(omaha3): Interactive updates might be useful for debugging or even
  // on-demand updates of all apps. Figure out how to expose this. For now, no
  // install source, which should not happen normally, is used as the trigger.
  // The simplest way to make this work is to set args_.is_silent_set
  // accordingly. However, we also need a way for this to work for per-machine
  // instances since IsMachineProcess() relies on being Local System. When we
  // settle on a mechanism, we should update the parser and remove this.
  if (args_.mode == COMMANDLINE_MODE_UA) {
    args_.is_silent_set = !args_.install_source.IsEmpty();
  }

  HRESULT hr = InitializeGoopdateAndLoadResources();
  if (FAILED(hr)) {
    CORE_LOG(LE,
             (_T("[InitializeGoopdateAndLoadResources failed][0x%08x]"), hr));
    if (internal::CanDisplayUi(args_.mode, args_.is_silent_set)) {
      // The resources are unavaliable, so we must use hard-coded text.
      const TCHAR* const kMsgBoxTitle = _T("Google Installer");
      CString message;
      message.Format(_T("Installation failed with error 0x%08x."), hr);
      VERIFY1(IDOK == ::MessageBox(NULL, message, kMsgBoxTitle, MB_OK));
    }
    return hr;
  }
  // The resources are now loaded and available if applicable for this instance.
  // If there was no bundle name specified on the command line, we take the
  // bundle name from the first app's name; if that has no name (or if there is
  // no apps, as in a runtime-only install) it will be an empty string.
  // Replace it with the localized installer name.
  if (args_.extra.bundle_name.IsEmpty()) {
    args_.extra.bundle_name.LoadString(IDS_PRODUCT_DISPLAY_NAME);
  }

  CORE_LOG(L2, (_T("[can use network %d]")
                _T("[can collect stats %d]"),
                ConfigManager::Instance()->CanUseNetwork(is_machine_),
                ConfigManager::Instance()->CanCollectStats(is_machine_)));

  bool has_ui_been_displayed = false;

  if (!is_machine_ && vista_util::IsElevatedWithUACMaybeOn()) {
    CORE_LOG(LW, (_T("User GoogleUpdate is possibly running in an unsupported ")
                  _T("way, at High integrity with UAC possibly enabled.")));
  }

  if (FAILED(parse_hr)) {
    ASSERT1(args_.mode == COMMANDLINE_MODE_UNKNOWN);
    hr = parse_hr;
  } else {
    ASSERT1(args_.mode != COMMANDLINE_MODE_UNKNOWN);
    // TODO(omaha): I would like to pass the mode as an argument, but there
    // are so many uses for args_.mode and they could easily creep in. Consider
    // eliminating the args_ member.
    hr = ExecuteMode(&has_ui_been_displayed);
    if (FAILED(hr)) {
      CORE_LOG(LE, (_T("[ExecuteMode failed][0x%08x]"), hr));
      // Continue and display error.
    }
  }

  if (FAILED(hr)) {
    HandleError(hr, has_ui_been_displayed);
    return hr;
  }

  return S_OK;
}

// Assumes the command line has been parsed.
HRESULT GoopdateImpl::InitializeGoopdateAndLoadResources() {
  // IsMachineProcess requires the command line be parsed first.
  is_machine_ = IsMachineProcess();
  OPT_LOG(L1, (_T("[is machine: %d]"), is_machine_));

  if (!::SetEnvironmentVariable(kEnvVariableIsMachine,
                                is_machine_ ? _T("1") : _T("0"))) {
    HRESULT hr = HRESULTFromLastError();
    CORE_LOG(LW, (_T("[::SetEnvironmentVariable failed][%s][0x%x]"),
                  kEnvVariableIsMachine, hr));
  }

  // After parsing the command line, reinstall the crash handler to match the
  // state of the process.
  if (is_machine_ != Crash::is_machine()) {
    VERIFY1(SUCCEEDED(Crash::InstallCrashHandler(is_machine_)));
  }

  // We have parsed the command line, and we are now resetting is_machine.
  NetworkConfigManager::set_is_machine(
      is_machine_ && vista_util::IsUserAdmin());

  // Set the current directory to be the one that the DLL was launched from.
  TCHAR module_directory[MAX_PATH] = {0};
  if (!GetModuleDirectory(module_instance_, module_directory)) {
     return HRESULTFromLastError();
  }
  if (!::SetCurrentDirectory(module_directory)) {
    return HRESULTFromLastError();
  }
  OPT_LOG(L3, (_T("[Current dir][%s]"), module_directory));

  // Set the usage stats as soon as possible, which is after the command line
  // has been parsed, so that we can report crashes and other stats.
  VERIFY1(SUCCEEDED(SetUsageStatsEnable()));

  VERIFY1(SUCCEEDED(internal::PromoteAppEulaAccepted(is_machine_)));

  if (ShouldCheckShutdownEvent(args_.mode) && IsShutdownEventSet()) {
    return GOOPDATE_E_SHUTDOWN_SIGNALED;
  }

  HRESULT hr = LoadResourceDllIfNecessary(args_.mode, module_directory);
  if (FAILED(hr)) {
    CORE_LOG(LE, (_T("[LoadResourceDllIfNecessary failed][0x%08x]"), hr));
    return hr;
  }

  return S_OK;
}

// Assumes Goopdate is initialized and resources are loaded.
HRESULT GoopdateImpl::ExecuteMode(bool* has_ui_been_displayed) {
  ASSERT1(has_ui_been_displayed);

  // Save the mode on the stack for post-mortem debugging purposes.
  volatile CommandLineMode mode = args_.mode;

  user_default_language_id_ = lang::GetDefaultLanguage(is_local_system_);

  ASSERT1(CheckRegisteredVersion(GetVersionString(), is_machine_, mode));

#pragma warning(push)
// C4061: enumerator 'xxx' in switch of enum 'yyy' is not explicitly handled by
// a case label.
#pragma warning(disable : 4061)
  switch (mode) {
    // Delegate to the service or the core. Both have reliability requirements
    // and resource constraints. Generally speaking, they do not use COM nor
    // networking code.
    case COMMANDLINE_MODE_SERVICE:
      return omaha::Update3ServiceModule().Main(SW_HIDE);

    case COMMANDLINE_MODE_MEDIUM_SERVICE:
      return omaha::UpdateMediumServiceModule().Main(SW_HIDE);

    case COMMANDLINE_MODE_SERVICE_REGISTER: {
      HRESULT hr = SetupUpdate3Service::InstallService(
                       app_util::GetModulePath(NULL));
      if (FAILED(hr)) {
        return hr;
      }
      return SetupUpdateMediumService::InstallService(
                 app_util::GetModulePath(NULL));
    }

    case COMMANDLINE_MODE_SERVICE_UNREGISTER: {
      HRESULT hr = SetupUpdate3Service::UninstallService();
      if (FAILED(hr)) {
        return hr;
      }
      return SetupUpdateMediumService::UninstallService();
    }

    default: {
      scoped_co_init init_com_apt(COINIT_MULTITHREADED);
      HRESULT hr = init_com_apt.hresult();
      if (FAILED(hr)) {
        return hr;
      }

      switch (mode) {
        case COMMANDLINE_MODE_CORE:
          return omaha::Core().Main(is_local_system_,
                                    !args_.is_crash_handler_disabled);

        case COMMANDLINE_MODE_CRASH_HANDLER:
          return omaha::CrashHandler().Main(is_local_system_);

        case COMMANDLINE_MODE_NOARGS:
          return GOOPDATE_E_NO_ARGS;

        case COMMANDLINE_MODE_UNREGISTER_PRODUCT:
          // TODO(omaha3): Eliminate the need for this mode.
          return E_FAIL;

        case COMMANDLINE_MODE_COMBROKER:
          return omaha::GoogleUpdate(is_machine_,
                                     omaha::GoogleUpdate::kBrokerMode).Main();

        case COMMANDLINE_MODE_ONDEMAND:
         return omaha::GoogleUpdate(
             is_machine_,
             omaha::GoogleUpdate::kOnDemandMode).Main();

        default: {
          // Reference the network instance here so the singleton can be
          // created before possible impersonation.
          NetworkConfigManager::Instance();

          switch (mode) {
            case COMMANDLINE_MODE_WEBPLUGIN:
              return HandleWebPlugin();

            case COMMANDLINE_MODE_CODE_RED_CHECK:
              return HandleCodeRedCheck();

            case COMMANDLINE_MODE_NETDIAGS:
              return NetDiags().Main();

            case COMMANDLINE_MODE_REGISTER_PRODUCT:
              // TODO(omaha3): Eliminate the need for this mode.
              return E_FAIL;

            case COMMANDLINE_MODE_INSTALL:
              return DoInstall(has_ui_been_displayed);

            case COMMANDLINE_MODE_UPDATE:
              return DoSelfUpdate();

            case COMMANDLINE_MODE_RECOVER:
              return DoRecover();

            case COMMANDLINE_MODE_HANDOFF_INSTALL:
              return DoHandoff(has_ui_been_displayed);

            case COMMANDLINE_MODE_UA:
              return DoUpdateAllApps(has_ui_been_displayed);

            case COMMANDLINE_MODE_CRASH:
              return DoCrash();

            case COMMANDLINE_MODE_REPORTCRASH:
              return HandleReportCrash();

            case COMMANDLINE_MODE_REGSERVER:
            case COMMANDLINE_MODE_UNREGSERVER: {
              hr = omaha::GoogleUpdate(
                  is_machine_, omaha::GoogleUpdate::kUpdate3Mode).Main();
              if (FAILED(hr)) {
                return hr;
              }
              hr = omaha::GoogleUpdate(
                  is_machine_, omaha::GoogleUpdate::kBrokerMode).Main();
              if (FAILED(hr)) {
                return hr;
              }
              return omaha::GoogleUpdate(
                  is_machine_, omaha::GoogleUpdate::kOnDemandMode).Main();
            }

            case COMMANDLINE_MODE_COMSERVER:
              return omaha::GoogleUpdate(
                  is_machine_, omaha::GoogleUpdate::kUpdate3Mode).Main();

            case COMMANDLINE_MODE_UNINSTALL:
              return HandleUninstall();

            case COMMANDLINE_MODE_PING:
              return HandlePing();

            default:
              // We have a COMMANDLINE_MODE_ that isn't being handled.
              ASSERT1(false);
              OPT_LOG(LE, (_T("[Command line has unhandled mode]")));
              return E_UNEXPECTED;
          }
        }
      }
    }
  }
#pragma warning(pop)
}

bool GoopdateImpl::IsMachineProcess() {
  Tristate needs_admin(TRISTATE_NONE);
  if (!args_.extra.apps.empty()) {
    needs_admin = args_.extra.apps[0].needs_admin != NEEDS_ADMIN_NO ?
        TRISTATE_TRUE : TRISTATE_FALSE;
  }

  return internal::IsMachineProcess(
      args_.mode,
      goopdate_utils::IsRunningFromOfficialGoopdateDir(true),
      is_local_system_,
      args_.is_machine_set,
      needs_admin);
}


HRESULT GoopdateImpl::HandleReportCrash() {
  ++metric_goopdate_handle_report_crash;
  VERIFY1(SUCCEEDED(AggregateMetrics(is_machine_)));

  // Catch exceptions to avoid reporting crashes when handling a crash.
  // TODO(omaha): maybe let Windows handle the crashes when reporting crashes
  // in certain interactive modes.
  HRESULT hr = S_OK;
  __try {
    // Crashes are uploaded always in the out-of-process case.
    //
    // ViaSat Update internal crashes are handled in-process. They are uploaded
    // only for the users that have opted in sending usage stats and when
    // network use is allowed, and from systems that are not development
    // nor test.
    // This default behavior can be overriden by an UpdatedDev parameter that
    // allows crashes to be uploaded always.
    //
    // All GoogleUpdate crashes are logged in the Windows event log for
    // applications, unless the logging is disabled by the administrator.
    ConfigManager* cm = ConfigManager::Instance();
    const bool can_upload_in_process = cm->AlwaysAllowCrashUploads() ||
        (cm->CanCollectStats(is_machine_) &&
         cm->CanUseNetwork(is_machine_)   &&
         !goopdate_utils::IsTestSource());
    hr = Crash::Report(can_upload_in_process,
                       args_.crash_filename,
                       args_.custom_info_filename,
                       user_default_language_id_);
  }
  __except(EXCEPTION_EXECUTE_HANDLER) {
    hr = E_FAIL;
  }
  return hr;
}

bool GoopdateImpl::ShouldCheckShutdownEvent(CommandLineMode mode) {
  switch (mode) {
    // Modes that may run before or during installation or otherwise do not
    // need to listen to shutdown.
    case COMMANDLINE_MODE_UNKNOWN:
    case COMMANDLINE_MODE_NOARGS:
    case COMMANDLINE_MODE_REGSERVER:
    case COMMANDLINE_MODE_UNREGSERVER:
    case COMMANDLINE_MODE_NETDIAGS:
    case COMMANDLINE_MODE_CRASH:
    case COMMANDLINE_MODE_REPORTCRASH:
    case COMMANDLINE_MODE_RECOVER:
    case COMMANDLINE_MODE_SERVICE_REGISTER:
    case COMMANDLINE_MODE_SERVICE_UNREGISTER:

    case COMMANDLINE_MODE_INSTALL:
    case COMMANDLINE_MODE_UPDATE:
    case COMMANDLINE_MODE_WEBPLUGIN:
    case COMMANDLINE_MODE_CODE_RED_CHECK:
    case COMMANDLINE_MODE_REGISTER_PRODUCT:
    case COMMANDLINE_MODE_UNREGISTER_PRODUCT:
    case COMMANDLINE_MODE_PING:
      return false;

    // Modes that should honor shutdown.
    case COMMANDLINE_MODE_CORE:
    case COMMANDLINE_MODE_SERVICE:
    case COMMANDLINE_MODE_COMSERVER:
    case COMMANDLINE_MODE_CRASH_HANDLER:
    case COMMANDLINE_MODE_COMBROKER:
    case COMMANDLINE_MODE_ONDEMAND:
    case COMMANDLINE_MODE_MEDIUM_SERVICE:

    case COMMANDLINE_MODE_HANDOFF_INSTALL:
    case COMMANDLINE_MODE_UA:
    case COMMANDLINE_MODE_UNINSTALL:
      return true;

    default:
      ASSERT1(false);
      return true;
  }
}

bool GoopdateImpl::IsShutdownEventSet() {
  NamedObjectAttributes attr;
  GetNamedObjectAttributes(kShutdownEvent, is_machine_, &attr);
  scoped_event shutdown_event(::OpenEvent(SYNCHRONIZE, false, attr.name));
  if (!shutdown_event) {
    return false;
  }

  return WAIT_OBJECT_0 == ::WaitForSingleObject(get(shutdown_event), 0);
}

// The resource dll is loaded only in the following cases:
// 1. Initial setup: /install
// 2. Handoff install: /handoff
// 3. App update worker: /ua
// 4. Various registrations.
// 5. Modes where an error message needs to be displayed.
HRESULT GoopdateImpl::LoadResourceDllIfNecessary(CommandLineMode mode,
                                                 const CString& resource_dir) {
  switch (mode) {
    case COMMANDLINE_MODE_UNKNOWN:             // Displays an error using UI.
    case COMMANDLINE_MODE_NOARGS:              // Displays an error using UI.
    case COMMANDLINE_MODE_INSTALL:             // Has UI on errors.
    case COMMANDLINE_MODE_UPDATE:              // Task and Service descriptions.
    case COMMANDLINE_MODE_RECOVER:             // Writes strings to registry.
    case COMMANDLINE_MODE_HANDOFF_INSTALL:     // Has optional UI.
    case COMMANDLINE_MODE_UA:                  // Has optional UI.
    case COMMANDLINE_MODE_COMSERVER:           // Returns strings to caller.
    case COMMANDLINE_MODE_SERVICE:             // Returns strings to caller.
    case COMMANDLINE_MODE_MEDIUM_SERVICE:      // TODO(omaha): Check & explain.
    case COMMANDLINE_MODE_SERVICE_REGISTER:    // Requires the RGS resources.
    case COMMANDLINE_MODE_SERVICE_UNREGISTER:  // Requires the RGS resources.
    case COMMANDLINE_MODE_ONDEMAND:            // Worker, etc. load strings.
      // Load the resource DLL for these modes.
      break;

    // For the Core, the resource DLL needs to be loaded when the Core is
    // servicing IGoogleUpdate3. The Core loads the resource DLL after the Code
    // Red kickoff, from within core.cc.
    case COMMANDLINE_MODE_CORE:
    case COMMANDLINE_MODE_REGSERVER:
    case COMMANDLINE_MODE_UNREGSERVER:
    case COMMANDLINE_MODE_NETDIAGS:
    case COMMANDLINE_MODE_CRASH:
    case COMMANDLINE_MODE_REPORTCRASH:
    case COMMANDLINE_MODE_WEBPLUGIN:
    case COMMANDLINE_MODE_CODE_RED_CHECK:
    case COMMANDLINE_MODE_REGISTER_PRODUCT:
    case COMMANDLINE_MODE_UNREGISTER_PRODUCT:
    case COMMANDLINE_MODE_CRASH_HANDLER:
    case COMMANDLINE_MODE_COMBROKER:
    case COMMANDLINE_MODE_UNINSTALL:
    case COMMANDLINE_MODE_PING:
    default:
      // These modes do not need the resource DLL.
      ASSERT1(!internal::CanDisplayUi(mode, false));
      return S_OK;
  }

  // TODO(omaha3): Consider not using ResourceManager in this file.
  HRESULT hr = ResourceManager::Create(
      is_machine_,
      resource_dir,
      lang::GetLanguageForProcess(args_.extra.language));
  if (FAILED(hr)) {
    ASSERT(false, (_T("ResourceManager::Create failed with 0x%08x"), hr));
    return hr;
  }

  return S_OK;
}

// Writes the information for the primary app to enable Omaha to send usage
// stats now. It will be set for each app in the args when they are installed.
HRESULT GoopdateImpl::SetUsageStatsEnable() {
  if (args_.extra.apps.empty()) {
    return S_OK;
  }

  HRESULT hr = app_registry_utils::SetUsageStatsEnable(
      args_.extra.apps[0].needs_admin != NEEDS_ADMIN_NO,
      GuidToString(args_.extra.apps[0].app_guid),
      args_.extra.usage_stats_enable);
  if (FAILED(hr)) {
    CORE_LOG(LW, (_T("[SetUsageStatsEnable failed][0x%08x]"), hr));

    if ((HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) == hr) &&
        args_.extra.apps[0].needs_admin &&
        !vista_util::IsUserAdmin()) {
      CORE_LOG(L3, (_T("[Process does not have permission to HKLM]")));
      return S_OK;
    }
    return hr;
  }

  return S_OK;
}

HRESULT GoopdateImpl::HandleCodeRedCheck() {
  CORE_LOG(L2, (_T("[GoopdateImpl::HandleCodeRedCheck]")));
  ++metric_cr_process_total;

  // Call the utils method instead of member method because we want to execute
  // as little code as possible.
  bool is_machine = internal::IsMachineProcess(args_.mode,
                                               false,   // machine dir
                                               is_local_system_,
                                               args_.is_machine_set,
                                               TRISTATE_NONE);
  ASSERT1(IsMachineProcess() == is_machine);

  if (!ConfigManager::Instance()->CanUseNetwork(is_machine)) {
    CORE_LOG(L1,
             (_T("[Code Red check not sent because network use prohibited]")));
    return GOOPDATE_E_CANNOT_USE_NETWORK;
  }

  CheckForCodeRed(is_machine, this_version_);
  return S_OK;
}

// Even though http://b/1135173 is fixed, there is still a possibility that only
// some of the files will be copied if Setup is currently running.
// TODO(omaha3): If we save and use the metainstaller for OneClick, that may
// address this.

// If we're called with the /webplugin command, we need to handle it and exit.
// This is called from the browser and the command line arguments come from the
// website so we need to be restrictive of what we let past. If everything from
// the plugin is valid, we'll relaunch goopdate with the proper commands.
HRESULT GoopdateImpl::HandleWebPlugin() {
  return webplugin_utils::DoOneClickInstall(args_);
}

HRESULT GoopdateImpl::DoInstall(bool* has_ui_been_displayed) {
  OPT_LOG(L1, (_T("[GoopdateImpl::DoInstall]")));
  ASSERT1(has_ui_been_displayed);

  // Some /install command lines, such as /silent /install will not necessarily
  // have an install source. To differentiate these from other sources, such as
  // other clients using the COM API, specify a generic source for /install.
  // TODO(omaha3): Updating two types of command line/args is undesirable, and
  // this does not use CommandLineBuilder. This could be addressed in several
  // ways. See the TODO above install.cc::LaunchHandoffProcess().
  ASSERT1(args_.mode == COMMANDLINE_MODE_INSTALL);
  CString install_command_line = cmd_line_;
  CommandLineArgs install_args = args_;
  if (args_.install_source.IsEmpty()) {
    ASSERT1(-1 == cmd_line_.Find(kCmdLineInstallSource));
    SafeCStringAppendFormat(&install_command_line, _T(" /%s %s"),
                            kCmdLineInstallSource,
                            kCmdLineInstallSource_InstallDefault);
    install_args.install_source = kCmdLineInstallSource_InstallDefault;
  }

  HRESULT hr = S_OK;
  if (args_.is_oem_set) {
    hr = OemInstall(!args_.is_silent_set,       // is_interactive
                    !args_.extra.runtime_only,  // is_app_install
                    args_.is_eula_required_set,
                    args_.is_install_elevated,
                    install_command_line,
                    install_args,
                    &is_machine_,
                    has_ui_been_displayed);
  } else {
    hr = Install(!args_.is_silent_set,          // is_interactive
                 !args_.extra.runtime_only,     // is_app_install
                 args_.is_eula_required_set,
                 false,
                 args_.is_install_elevated,
                 install_command_line,
                 install_args,
                 &is_machine_,
                 has_ui_been_displayed);
  }

  if (FAILED(hr)) {
    CORE_LOG(LE, (_T("[Install failed][0x%08x]"), hr));
    return hr;
  }

  return S_OK;
}

HRESULT GoopdateImpl::DoSelfUpdate() {
  OPT_LOG(L1, (_T("[GoopdateImpl::DoSelfUpdate]")));

  HRESULT hr = install_self::UpdateSelf(is_machine_, args_.session_id);
  if (FAILED(hr)) {
    CORE_LOG(LE, (_T("[UpdateSelf failed][0x%08x]"), hr));
    return hr;
  }

  return S_OK;
}

// Attempts to launch the repair file elevated using the MSP. If elevation is
// not needed or fails, attempts to install without elevating.
// The code_red_metainstaller_path arg is the repair file.
HRESULT GoopdateImpl::DoRecover() {
  OPT_LOG(L1, (_T("[GoopdateImpl::DoRecover()]")));

// TODO(omaha3): Enable. Maybe build without the builder.
#if 0
  CommandLineBuilder builder(COMMANDLINE_MODE_UPDATE);

  HRESULT hr = S_OK;
  if (LaunchRepairFileElevated(is_machine_,
                               args_.code_red_metainstaller_path,
                               builder.GetCommandLineArgs(),
                               &hr)) {
    ASSERT1(SUCCEEDED(hr));
    return S_OK;
  }
#else
  HRESULT hr = S_OK;
#endif

  if (FAILED(hr)) {
    OPT_LOG(LW, (_T("[LaunchRepairFileElevated failed][0x%08x]"), hr));
  }

  hr = install_self::Repair(is_machine_);
  if (FAILED(hr)) {
    OPT_LOG(LE, (_T("[Non-elevated repair failed][0x%08x]"), hr));
    return hr;
  }

  return S_OK;
}

// Clears the EULA flag in the handoff instance in case an older installer that
// does not know about the EULA flag is used to launch the install.
// Failing to clear flag fails installation because this would prevent updates.
HRESULT GoopdateImpl::DoHandoff(bool* has_ui_been_displayed) {
  OPT_LOG(L1, (_T("[GoopdateImpl::DoHandoff]")));
  ASSERT1(has_ui_been_displayed);

  if (!args_.is_eula_required_set) {
    HRESULT hr = install_self::SetEulaAccepted(is_machine_);
    if (FAILED(hr)) {
      CORE_LOG(LE, (_T("[install_self::SetEulaAccepted failed][0x%08x]"), hr));
      return hr;
    }
  }

  HRESULT hr = InitializeClientSecurity();
  if (FAILED(hr)) {
    return hr;
  }

  // If /sessionid wasn't specified on the command line, generate a random GUID
  // to use for the session ID.  (This can happen if a metainstaller from the
  // prior version does a handoff to a newer version.)
  CString session_id = args_.session_id;
  if (session_id.IsEmpty()) {
    VERIFY1(SUCCEEDED(GetGuid(&session_id)));
  }

  hr = InstallApps(is_machine_,
                   !args_.is_silent_set,  // is_interactive.
                   !args_.is_eula_required_set,  // is_eula_accepted.
                   args_.is_oem_set,
                   args_.is_offline_set,
                   args_.offline_dir,
                   args_.extra,
                   args_.install_source,
                   session_id,
                   has_ui_been_displayed);
  if (FAILED(hr)) {
    CORE_LOG(LE, (_T("[Install failed][0x%08x]"), hr));
    return hr;
  }

  return S_OK;
}

HRESULT GoopdateImpl::DoUpdateAllApps(bool* has_ui_been_displayed ) {
  OPT_LOG(L1, (_T("[GoopdateImpl::DoUpdateAllApps]")));
  ASSERT1(has_ui_been_displayed);

  bool is_interactive_update = !args_.is_silent_set;

  // TODO(omaha3): Interactive is used as an indication of an on-demand request.
  // It might also be useful to allow on-demand silent update requests.
  // This was a request when we added the ability to disable updates.
  // TODO(omaha3): These on-demand requests should also allow updates if the
  // app update policy is set to on-demand in addition to silent auto.
  const bool is_on_demand = is_interactive_update;

  CString install_source = args_.install_source;

  if (is_on_demand && install_source.IsEmpty()) {
    // Set an install source for interactive/on-demand update all apps.
    install_source = kCmdLineInstallSource_OnDemandUA;
  }

  // TODO(omaha): Consider moving InitializeClientSecurity calls inside
  // install_apps.cc or maybe to update3_utils::CreateGoogleUpdate3Class().
  HRESULT hr = InitializeClientSecurity();
  if (FAILED(hr)) {
    ASSERT1(false);
    return is_interactive_update ? hr : S_OK;
  }

  hr = UpdateApps(is_machine_,
                  is_interactive_update,
                  is_on_demand,
                  install_source,
                  args_.extra.language,
                  has_ui_been_displayed);
  OPT_LOG(L2, (_T("[Update all apps process finished][0x%x]"), hr));

  // The UA worker always returns S_OK. UA can be launched by the scheduled task
  // or the core, neither of which wait for a return code. On Vista, returning
  // an error code from the scheduled task reportedly causes issues with the
  // task scheduler in rare cases, so returning S_OK helps with that as well.
  // However, in interactive cases, we should return the actual error so that it
  // can be reported to the user if necessary.
  return is_interactive_update ? hr : S_OK;
}

HRESULT GoopdateImpl::DoCrash() {
#if DEBUG
  return static_cast<HRESULT>(Crash::CrashNow());
#else
  return S_OK;
#endif
}

HRESULT GoopdateImpl::HandleUninstall() {
  // TODO(omaha3): Why don't we always acquire this lock before uninstalling?
  // We don't in the /install case. I guess it's important that we uninstall
  // the first time in that case, but there's a chance that the case in the
  // referenced bug could occur while a user is installing.

  // Attempt a conditional uninstall and always return S_OK to avoid
  // executing error handling code in the case of an actual uninstall.
  // Do not attempt to uninstall if MSI is busy to avoid spurious uninstalls.
  // See http://b/1436223. The call to WaitForMSIExecute blocks with a
  // timeout. It is better to block here than block while holding the setup
  // lock.
  HRESULT hr = WaitForMSIExecute(kWaitForMSIExecuteMs);
  CORE_LOG(L2, (_T("[WaitForMSIExecute returned 0x%08x]"), hr));
  if (SUCCEEDED(hr)) {
    MaybeUninstallGoogleUpdate();
  }
  return S_OK;
}

HRESULT GoopdateImpl::HandlePing() {
  return Ping::HandlePing(is_machine_, args_.ping_string);
}

// TODO(omaha3): In Omaha 2, this was also called when /ig failed. There is a
// separate call to UninstallSelf for /install in goopdate.cc. Should we call
// this instead to ensure we ping? Should we try to only call from one location?
// If we move it, make sure it is called regardless of LastChecked. See
// http://b/2663423.

// Uninstall is a tricky use case. Uninstall can primarily happen in three cases
// and there are two mechanisms to uninstall. The cases in which Omaha
// uninstalls are:
// 1. The last registered application uninstalls. If Omaha is long-running,
// Omaha monitors the Client keys and it will trigger an immediate uninstall in
// this case.
// 2. The core starts an update worker, if there are no registered
// applications, the update worker will do the uninstall.
// 3. An error, including user cancel, happens during Omaha or app installation
// and there are no registered applications.
// The uninstall is implemented in terms of the following mechanisms:
// * An update worker launched with "/ua /uninstalled" by the core, in the
// first two cases above.
// * A direct uninstall, in the case of errors or user cancellations, in the
// last case above.
//
// Omaha can uninstall only if there are no install workers running and no
// registered applications. This check is done under the setup lock protection.
// In addition, the uninstall worker takes the update worker lock. Acquiring
// this lock is important since the silent installers can modify the
// registration of apps and trigger uninstalls workers. Therefore, both
// setup lock and the update worker locks are needed.
//
// In the direct uninstall case there is a small race condition, since there is
// no other single lock that can be acquired to prevent changes to the
// application registration. The code looks for install workers but the test is
// racy if not protected by locks.
void GoopdateImpl::MaybeUninstallGoogleUpdate() {
  CORE_LOG(L1, (_T("[MaybeUninstallGoogleUpdate]")));
  has_uninstalled_ =
      !!SUCCEEDED(install_self::UninstallSelf(is_machine_, true));
}

// In dbg builds, also checks the post conditions of a /install process to
// ensure that the registry is correctly populated or has been cleaned up.
HRESULT GoopdateImpl::UninstallIfNecessary() {
  ASSERT1(COMMANDLINE_MODE_INSTALL == args_.mode);
  ASSERT(!has_uninstalled_, (_T("Worker doesn't uninstall in /install mode")));

  if (is_machine_ && !vista_util::IsUserAdmin()) {
    // The non-elevated instance, so do not try to uninstall.
    return S_OK;
  } else {
    CORE_LOG(L2, (_T("[GoopdateImpl::Main /install][Uninstall if necessary]")));
    // COM must be initialized in order to uninstall the scheduled task(s).
    scoped_co_init init_com_apt(COINIT_MULTITHREADED);
    return install_self::UninstallSelf(is_machine_, false);
  }
}

void GoopdateImpl::OutOfMemoryHandler() {
  ::RaiseException(EXCEPTION_ACCESS_VIOLATION,
                   EXCEPTION_NONCONTINUABLE,
                   0,
                   NULL);
}

HRESULT GoopdateImpl::CaptureOSMetrics() {
  int major_version(0);
  int minor_version(0);
  int service_pack_major(0);
  int service_pack_minor(0);
  TCHAR name[MAX_PATH] = {0};

  HRESULT hr = SystemInfo::GetSystemVersion(&major_version,
                                            &minor_version,
                                            &service_pack_major,
                                            &service_pack_minor,
                                            name,
                                            arraysize(name));
  if (SUCCEEDED(hr)) {
    metric_windows_major_version    = major_version;
    metric_windows_minor_version    = minor_version;
    metric_windows_sp_major_version = service_pack_major;
    metric_windows_sp_minor_version = service_pack_minor;
  }

  return hr;
}

}  // namespace detail

namespace internal {

// TODO(omaha3): Consider moving to app_registry_utils.
// Returns early if ViaSat Update's EULA is already accepted, as indicated by
// the lack of eulaaccepted in the Update key.
// The apps' values are not modified or deleted.
HRESULT PromoteAppEulaAccepted(bool is_machine) {
  const TCHAR* update_key_name =
      ConfigManager::Instance()->registry_update(is_machine);
  if (!RegKey::HasValue(update_key_name, kRegValueOmahaEulaAccepted)) {
    return S_OK;
  }

  const TCHAR* state_key_name =
      ConfigManager::Instance()->registry_client_state(is_machine);

  RegKey state_key;
  HRESULT hr = state_key.Open(state_key_name, KEY_READ);
  if (FAILED(hr)) {
    if (HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) == hr) {
      return S_FALSE;
    }
    return hr;
  }

  // TODO(omaha): This should actually be iterating over registered products
  // rather than present ClientState keys. These are identical in most cases.
  int num_sub_keys = state_key.GetSubkeyCount();
  for (int i = 0; i < num_sub_keys; ++i) {
    CString sub_key_name;
    if (FAILED(state_key.GetSubkeyNameAt(i, &sub_key_name))) {
      continue;
    }

    if (app_registry_utils::IsAppEulaAccepted(is_machine, sub_key_name, true)) {
      ASSERT1(kGoogleUpdateAppId != sub_key_name);
      return install_self::SetEulaAccepted(is_machine);
    }
  }

  return S_OK;
}

// TODO(omaha): Use a registry override instead.
#if !OFFICIAL_BUILD
bool IsOmahaShellRunningFromStaging() {
  return !app_util::GetModuleName(NULL).CompareNoCase(kOmahaShellFileName) &&
         app_util::GetModuleDirectory(NULL).Right(_tcslen(_T("staging"))) ==
         _T("staging");
}
#endif

// TODO(omaha): needs_admin is only used for one case. Can we avoid it?
bool IsMachineProcess(CommandLineMode mode,
                      bool is_running_from_official_machine_directory,
                      bool is_local_system,
                      bool is_machine_override,
                      Tristate needs_admin) {
  switch (mode) {
    // These "install" operations may not be running from the installed
    // location.
    case COMMANDLINE_MODE_INSTALL:
    case COMMANDLINE_MODE_HANDOFF_INSTALL:
    case COMMANDLINE_MODE_REGISTER_PRODUCT:
    case COMMANDLINE_MODE_UNREGISTER_PRODUCT:
      ASSERT1(TRISTATE_NONE != needs_admin);
      return TRISTATE_TRUE == needs_admin;

    // The following is a Code Red repair executable, which runs from temp dir.
    case COMMANDLINE_MODE_RECOVER:
      return is_machine_override;

    // The following always runs as the user and may provide UI for on-demand
    // installs and browser launches.
    // The install location determines user vs. machine.
    case COMMANDLINE_MODE_COMSERVER:
    case COMMANDLINE_MODE_ONDEMAND:
#if !OFFICIAL_BUILD
      // Return machine == true. This is to facilitate unit tests such as
      // GoogleUpdateCoreTest.LaunchCmdElevated_LocalServerRegistered.
      if (IsOmahaShellRunningFromStaging()) {
        return true;
      }
#endif

      ASSERT1(goopdate_utils::IsRunningFromOfficialGoopdateDir(false) ||
              goopdate_utils::IsRunningFromOfficialGoopdateDir(true) ||
              _T("omaha_unittest.exe") == app_util::GetCurrentModuleName() ||
              _T("GoogleUpdate_unsigned.exe") ==
                  app_util::GetModuleName(NULL));  // Running in debugger.
      return is_running_from_official_machine_directory;

    // The broker forwarder is elevatable and always runs as the user it was
    // created as.
    case COMMANDLINE_MODE_COMBROKER:
      return is_running_from_official_machine_directory;

    // The following always runs as the user and is user-initiated.
    case COMMANDLINE_MODE_WEBPLUGIN:
      // The install location determines user vs. machine.
      // This may not be the desired value when doing a cross-install or using
      // the opposite plugin (i.e. user plugin is often used before the machine
      // one).
      ASSERT1(goopdate_utils::IsRunningFromOfficialGoopdateDir(false) ||
              goopdate_utils::IsRunningFromOfficialGoopdateDir(true) ||
              _T("omaha_unittest.exe") == app_util::GetCurrentModuleName());
      return is_running_from_official_machine_directory;

    // The following all run silently as the user for user installs or Local
    // System for machine installs.
    case COMMANDLINE_MODE_UPDATE:
    case COMMANDLINE_MODE_CODE_RED_CHECK:
      return is_local_system;

    // The following all run silently as the user for user installs or Local
    // System for machine installs.
    case COMMANDLINE_MODE_UA:
      return is_local_system ? true : is_machine_override;

    // /ua runs silently as the user for user installs or Local System for
    // machine installs. Interactive machine /ua may be run as the user if
    // /machine is specified.
    case COMMANDLINE_MODE_CORE:
    case COMMANDLINE_MODE_CRASH_HANDLER:
      return is_local_system;

    // The following runs silently as Local System.
    case COMMANDLINE_MODE_SERVICE:
    case COMMANDLINE_MODE_MEDIUM_SERVICE:
      ASSERT1(is_local_system);
      return is_local_system;

    // The following run as machine for all installs.
    case COMMANDLINE_MODE_SERVICE_REGISTER:
    case COMMANDLINE_MODE_SERVICE_UNREGISTER:
      return true;

    // The crashing process determines whether it was a machine or user omaha
    // and correctly sets the /machine switch.
    case COMMANDLINE_MODE_REPORTCRASH:
      return is_machine_override;

    // The following all run silently as the user for all installs.
    case COMMANDLINE_MODE_REGSERVER:
    case COMMANDLINE_MODE_UNREGSERVER:
#if !OFFICIAL_BUILD
      // Return machine == true. This is to facilitate unit tests such as
      // GoogleUpdateCoreTest.LaunchCmdElevated_LocalServerRegistered.
      if (IsOmahaShellRunningFromStaging()) {
        return true;
      }
#endif

      ASSERT1(goopdate_utils::IsRunningFromOfficialGoopdateDir(false) ||
              goopdate_utils::IsRunningFromOfficialGoopdateDir(true) ||
              _T("omaha_unittest.exe") == app_util::GetCurrentModuleName());
      return is_running_from_official_machine_directory;

    // The following may run as user or Local System. Thus, use the directory.
    case COMMANDLINE_MODE_UNINSTALL:
    case COMMANDLINE_MODE_PING:
      ASSERT1(goopdate_utils::IsRunningFromOfficialGoopdateDir(false) ||
              goopdate_utils::IsRunningFromOfficialGoopdateDir(true) ||
              _T("omaha_unittest.exe") == app_util::GetCurrentModuleName());
      return is_running_from_official_machine_directory;

    // The following are miscellaneous modes that we do not expect to be running
    // in the wild.
    case COMMANDLINE_MODE_NOARGS:
    case COMMANDLINE_MODE_UNKNOWN:
    case COMMANDLINE_MODE_NETDIAGS:
    case COMMANDLINE_MODE_CRASH:
    default:
      return is_running_from_official_machine_directory;
  }
}

bool CanDisplayUi(CommandLineMode mode, bool is_silent) {
  switch (mode) {
    case COMMANDLINE_MODE_UNKNOWN:
      // This mode is not one of our known silent modes. Therefore, we can
      // display an error UI.
      return true;

    case COMMANDLINE_MODE_INSTALL:
    case COMMANDLINE_MODE_HANDOFF_INSTALL:
    case COMMANDLINE_MODE_UA:
      // These modes have UI unless they are silent.
      // UA is usually silent, but follows the same logic.
      return !is_silent;

    case COMMANDLINE_MODE_NOARGS:
    case COMMANDLINE_MODE_CORE:
    case COMMANDLINE_MODE_SERVICE:
    case COMMANDLINE_MODE_REGSERVER:
    case COMMANDLINE_MODE_UNREGSERVER:
    case COMMANDLINE_MODE_NETDIAGS:
    case COMMANDLINE_MODE_CRASH:
    case COMMANDLINE_MODE_REPORTCRASH:
    case COMMANDLINE_MODE_UPDATE:
    case COMMANDLINE_MODE_RECOVER:
    case COMMANDLINE_MODE_WEBPLUGIN:
    case COMMANDLINE_MODE_CODE_RED_CHECK:
    case COMMANDLINE_MODE_COMSERVER:
    case COMMANDLINE_MODE_REGISTER_PRODUCT:
    case COMMANDLINE_MODE_UNREGISTER_PRODUCT:
    case COMMANDLINE_MODE_SERVICE_REGISTER:
    case COMMANDLINE_MODE_SERVICE_UNREGISTER:
    case COMMANDLINE_MODE_CRASH_HANDLER:
    case COMMANDLINE_MODE_COMBROKER:
    case COMMANDLINE_MODE_ONDEMAND:
    case COMMANDLINE_MODE_MEDIUM_SERVICE:
    case COMMANDLINE_MODE_UNINSTALL:
    case COMMANDLINE_MODE_PING:
    default:
      // These modes are always silent.
      return false;
  }
}

}  // namespace internal

Goopdate* Goopdate::instance_              = NULL;

Goopdate& Goopdate::Instance() {
  ASSERT1(instance_);
  return *instance_;
}

Goopdate::Goopdate(bool is_local_system) {
  CORE_LOG(L2, (_T("[Goopdate::Goopdate]")));
  impl_.reset(new detail::GoopdateImpl(this, is_local_system));

  ASSERT1(!instance_);
  instance_ = this;
}

Goopdate::~Goopdate() {
  CORE_LOG(L2, (_T("[Goopdate::~Goopdate]")));

  Stop();

  instance_ = NULL;
}

HRESULT Goopdate::Main(HINSTANCE instance,
                       const TCHAR* cmd_line,
                       int cmd_show) {
  return impl_->Main(instance, cmd_line, cmd_show);
}

HRESULT Goopdate::QueueUserWorkItem(UserWorkItem* work_item, uint32 flags) {
  return impl_->QueueUserWorkItem(work_item, flags);
}

void Goopdate::Stop() {
  return impl_->Stop();
}

bool Goopdate::is_local_system() const {
  return impl_->is_local_system();
}

}  // namespace omaha
