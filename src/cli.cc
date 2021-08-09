#include "cli.hh"
#include "process.hh"
#include "common.hh"

#ifdef _WIN32
#include <shlwapi.h>
#include <strsafe.h>
#include <comdef.h>
#include <AppxPackaging.h>
#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Urlmon.lib")
#endif

constexpr auto version = STR_VALUE(VERSION);

using namespace Opkit;

void help () {
  std::cout
    << "opkit " << version
    << std::endl
    << std::endl
    << "usage:" << std::endl
    << "  opkit <project-dir> [-h, ...]"
    << std::endl
    << std::endl
    << "flags:" << std::endl
    << "  -b  bundle for app store" << std::endl
    << "  -c  code sign the bundle" << std::endl
    << "  -h  help" << std::endl
    << "  -me (macOS) use entitlements" << std::endl
    << "  -mn (macOS) notarize the bundle" << std::endl
    << "  -o  only run user build step" << std::endl
    << "  -p  package the app" << std::endl
    << "  -r  run after building" << std::endl
    << "  -xd turn off debug mode" << std::endl
  ;

  exit(0);
}

auto start = std::chrono::system_clock::now();

void log (const std::string s) {
#ifdef _WIN32 // unicode console support
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 1000);
#endif
  using namespace std::chrono;

  auto now = system_clock::now();
  auto delta = duration_cast<milliseconds>(now - start).count();
  std::cout << "• " << s << " \033[0;32m+" << delta << "ms\033[0m" << std::endl;
  start = std::chrono::system_clock::now();
}

static std::string getCxxFlags() {
  auto flags = getEnv("CXX_FLAGS");
  return flags.size() > 0 ? " " + flags : "";
}

int main (const int argc, const char* argv[]) {
  if (argc < 2) {
    help();
  }

  if (getEnv("CXX").size() == 0) {
    log("warning! $CXX env var not set, assuming defaults");

    if (platform.win) {
      setEnv("CXX=clang++");
    } else {
      setEnv("CXX=/usr/bin/g++");
    }
  }

  bool flagRunUserBuild = false;
  bool flagAppStore = false;
  bool flagCodeSign = false;
  bool flagShouldRun = false;
  bool flagEntitlements = false;
  bool flagShouldNotarize = false;
  bool flagDebugMode = true;
  bool flagShouldPackage = false;

  for (auto const arg : std::span(argv, argc)) {
    if (std::string(arg).find("-c") != -1) {
      flagCodeSign = true;
    }

    if (std::string(arg).find("-h") != -1) {
      help();
    }

    if (std::string(arg).find("-me") != -1) {
      flagEntitlements = true;
    }

    if (std::string(arg).find("-mn") != -1) {
      flagShouldNotarize = true;
    }

    if (std::string(arg).find("-o") != -1) {
      flagRunUserBuild = true;
    }

    if (std::string(arg).find("-p") != -1) {
      flagShouldPackage = true;
    }

    if (std::string(arg).find("-r") != -1) {
      flagShouldRun = true;
    }

    if (std::string(arg).find("-s") != -1) {
      flagAppStore = true;
    }

    if (std::string(arg).find("-xd") != -1) {
      flagDebugMode = false;
    }
  }

  //
  // TODO(@heapwolf) split path values from the settings file
  // on the os separator to make them work cross-platform.
  //

  auto target = fs::path(argv[1]);

  auto _settings = WStringToString(readFile(fs::path { target / "settings.config" }));
  auto settings = parseConfig(_settings);

  if (settings.count("_cmd") == 0) {
    log("at least one of 'win_cmd', 'mac_cmd', 'linux_cmd' key/value is required");
    exit(1);
  }

  std::vector<std::string> required = {
    "name",
    "title",
    "executable",
    "output",
    "version",
    "arch"
  };

  for (const auto &str : required) {
    if (settings.count(str) == 0) {
      log("'" + str + "' key/value is required");
      exit(1);
    }
  }

  if (flagDebugMode) {
    settings["name"] += "-dev";
    settings["title"] += "-dev";
    settings["executable"] += "-dev";
  }

  auto pathOutput = fs::path { settings["output"] };

  if (flagRunUserBuild == false) {
    fs::remove_all(pathOutput);
    log(std::string("cleaned: " + pathToString(pathOutput)));
  }

  auto executable = fs::path(
    settings["executable"] + (platform.win ? ".exe" : ""));

  std::string flags;
  std::string files;

  fs::path pathBin;
  fs::path pathResources;
  fs::path pathResourcesRelativeToUserBuild;
  fs::path pathPackage;
  fs::path pathToArchive;
  fs::path packageName;

  //
  // Darwin Package Prep
  // ---
  //
  if (platform.mac) {
    log("preparing build for mac");
    flags = "-std=c++2a -framework WebKit -framework Cocoa -ObjC++";
    flags += getCxxFlags();

    files += prefixFile("src/main.cc");
    files += prefixFile("src/process_unix.cc");

    fs::path pathBase = "Contents";
    packageName = fs::path(std::string(settings["name"] + ".app"));

    pathPackage = { target / pathOutput / packageName };
    pathBin = { pathPackage / pathBase / "MacOS" };
    pathResources = { pathPackage / pathBase / "Resources" };

    pathResourcesRelativeToUserBuild = {
      settings["output"] /
      packageName /
      pathBase /
      "Resources"
    };

    fs::create_directories(pathBin);
    fs::create_directories(pathResources);

    auto plistInfo = tmpl(gPListInfo, settings);

    writeFile(fs::path {
      pathPackage /
      pathBase /
      "Info.plist"
    }, plistInfo);
  }

  //
  // Linux Package Prep
  // ---
  //
  if (platform.linux) {
    log("preparing build for linux");
    flags = "-std=c++2a `pkg-config --cflags --libs gtk+-3.0 webkit2gtk-4.0`";
    flags += getCxxFlags();

    files += prefixFile("src/main.cc");
    files += prefixFile("src/process_unix.cc");

    // this follows the .deb file naming convention
    packageName = fs::path((
      settings["executable"] + "_" +
      settings["version"] + "-" +
      settings["revision"] + "_" +
      settings["arch"]
    ));

    pathPackage = { target / pathOutput / packageName };

    fs::path pathBase = {
      pathPackage /
      "opt" /
      settings["name"]
    };

    pathBin = pathBase;
    pathResources = pathBase;

    pathResourcesRelativeToUserBuild = {
      pathOutput /
      packageName /
      "opt" /
      settings["name"]
    };

    fs::path pathControlFile = {
      pathPackage /
      "DEBIAN"
    };

    fs::path pathManifestFile = {
      pathPackage /
      "usr" /
      "share" /
      "applications"
    };

    fs::path pathIcons = {
      pathPackage /
      "usr" /
      "share" /
      "icons" /
      "hicolor" /
      "256x256" /
      "apps"
    };

    fs::create_directories(pathIcons);
    fs::create_directories(pathResources);
    fs::create_directories(pathManifestFile);
    fs::create_directories(pathControlFile);

    auto linuxExecPath = fs::path {
      fs::path("/opt") /
      settings["name"] /
      settings["executable"]
    };

    settings["linux_executable_path"] = pathToString(linuxExecPath);
    settings["linux_icon_path"] = pathToString(fs::path {
      fs::path("/usr") /
      "share" /
      "icons" /
      "hicolor" /
      "256x256" /
      "apps" /
      (settings["executable"] + ".png")
    });

    writeFile(fs::path {
      pathManifestFile /
      (settings["name"] + ".desktop")
    }, tmpl(gDestkopManifest, settings));

    writeFile(fs::path {
      pathControlFile /
      "control"
    }, tmpl(gDebianManifest, settings));

    auto pathToIconSrc = pathToString(fs::path {
      target /
      settings["linux_icon"]
    });

    auto pathToIconDest = pathToString(fs::path {
      pathIcons /
      (settings["executable"] + ".png")
    });

    if (!fs::exists(pathToIconDest)) {
      fs::copy(pathToIconSrc, pathToIconDest);
    }
  }

  //
  // Windows Package Prep
  // ---
  //
  if (platform.win) {
    log("preparing build for win");
    auto prefix = prefixFile();

    flags = " -std=c++20"
      " -I" + prefix +
      " -I" + prefix + "\\src\\win64"
      " -L" + prefix + "\\src\\win64"
    ;

    files += prefixFile("src\\main.cc");
    files += prefixFile("src\\process_win.cc");

    packageName = fs::path((
      settings["executable"] + "-" +
      settings["version"]
    ));

    pathPackage = { fs::current_path() / target / pathOutput / packageName };
    pathBin = pathPackage;

    pathResourcesRelativeToUserBuild = pathPackage;

    fs::create_directories(pathPackage);

    auto p = fs::path {
      pathResourcesRelativeToUserBuild /
      "AppxManifest.xml"
    };

    if (settings["revision"].size() == 0) {
      settings["revision"] = "1";
    }

    writeFile(p, tmpl(gWindowsAppManifest, settings));

    // TODO Copy the files into place
  }

  log("package prepared");

  //
  // cd into the target and run the user's build command,
  // pass it the platform specific directory where they
  // should send their build artifacts.
  //
  std::stringstream buildCommand;
  auto oldCwd = std::filesystem::current_path();
  std::filesystem::current_path(fs::path { oldCwd / target });

  buildCommand
    << settings["build"]
    << " "
    << pathToString(pathResourcesRelativeToUserBuild)
    << " --debug=" << flagDebugMode;

  log(buildCommand.str());
  auto r = exec(buildCommand.str().c_str());

  if (r.exitCode != 0) {
    log("Unable to run user build command");
    exit(r.exitCode);
  }

  log(r.output);
  log("ran user build command");

  std::filesystem::current_path(oldCwd);

  std::stringstream compileCommand;
  fs::path binaryPath = { pathBin / executable };

  // Serialize the settings and strip the comments so that we can pass
  // them to the compiler by replacing new lines with a high bit.
  _settings = encodeURIComponent(_settings);

  auto extraFlags = flagDebugMode ? settings["debug_flags"] : settings["flags"];

  compileCommand
    << getEnv("CXX")
    << " " << files
    << " " << flags
    << " " << extraFlags
    << " -o " << pathToString(binaryPath)
    << " -DDEBUG=" << (flagDebugMode ? 1 : 0)
    << " -DSETTINGS=\"" << _settings << "\""
  ;

  // log(compileCommand.str());

  auto binExists = fs::exists(binaryPath);
  if (flagRunUserBuild == false || !binExists) {
    auto r = exec(compileCommand.str());

    if (r.exitCode != 0) {
      log("Unable to build");
      exit(r.exitCode);
    }

    log("compiled native binary");
  }

  //
  // Linux Packaging
  // ---
  //
  if (flagShouldPackage && platform.linux) {
    fs::path pathSymLinks = {
      pathPackage /
      "usr" /
      "local" /
      "bin"
    };

    auto linuxExecPath = fs::path {
      fs::path("/opt") /
      settings["name"] /
      settings["executable"]
    };

    fs::create_directories(pathSymLinks);
    fs::create_symlink(
      linuxExecPath,
      fs::path { pathSymLinks / settings["executable"] }
    );

    std::stringstream archiveCommand;

    archiveCommand
      << "dpkg-deb --build --root-owner-group "
      << pathToString(pathPackage)
      << " "
      << pathToString(fs::path { target / pathOutput });

    auto r = std::system(archiveCommand.str().c_str());

    if (r != 0) {
      log("error: failed to create deb package");
      exit(1);
    }
  }

  //
  // MacOS Code Signing
  // ---
  //
  if (flagCodeSign && platform.mac) {
    //
    // https://www.digicert.com/kb/code-signing/mac-os-codesign-tool.htm
    // https://developer.apple.com/forums/thread/128166
    // https://wiki.lazarus.freepascal.org/Code_Signing_for_macOS
    //
    std::stringstream signCommand;
    std::string entitlements = "";

    if (flagEntitlements) {
      auto entitlementsPath = fs::path {
        pathResourcesRelativeToUserBuild /
        "entitlements.plist"
      };

      fs::copy(
        fs::path { target / settings["mac_entitlements"] },
        entitlementsPath
      );

      entitlements = std::string(
        " --entitlements " + pathToString(entitlementsPath)
      );
    }

    //
    // TODO @heapwolf: DRY this up.
    //
    if (settings["mac_sign_paths"].size() > 0) {
      auto paths = split(settings["mac_sign_paths"], ';');

      for (int i = 0; i < paths.size(); i++) {
        std::string prefix = (i > 0) ? ";" : "";

        signCommand
          << prefix
          << " codesign"
          << " --force"
          << " --options runtime"
          << " --timestamp"
          << entitlements
          << " --sign 'Developer ID Application: " + settings["mac_sign"] + "'"
          << " "
          << pathToString(fs::path { pathResources / paths[i] })
        ;
      }
    }

    signCommand
      << "; codesign"
      << " --force"
      << " --options runtime"
      << " --timestamp"
      << entitlements
      << " --sign 'Developer ID Application: " + settings["mac_sign"] + "'"
      << " "
      << pathToString(fs::path { pathBin / executable })

      << "; codesign"
      << " --force"
      << " --options runtime"
      << " --timestamp"
      << entitlements
      << " --sign 'Developer ID Application: " + settings["mac_sign"] + "'"
      << " "
      << pathToString(pathPackage);

    // log(signCommand.str());
    auto r = exec(signCommand.str());

    if (r.exitCode != 0) {
      log("Unable to sign");
      exit(r.exitCode);
    }

    log("finished code signing");
  }

  //
  // MacOS Packaging
  // ---
  //
  if (flagShouldPackage && platform.mac) {
    std::stringstream zipCommand;

    pathToArchive = fs::path {
      target /
      pathOutput /
      (settings["executable"] + ".zip")
    };

    zipCommand
      << "ditto"
      << " -c"
      << " -k"
      << " --sequesterRsrc"
      << " --keepParent"
      << " "
      << pathToString(pathPackage)
      << " "
      << pathToString(pathToArchive);

    auto r = std::system(zipCommand.str().c_str());

    if (r != 0) {
      log("error: failed to create zip for notarization");
      exit(1);
    }

    log("craeted zip artifact");
  }

  //
  // MacOS Notorization
  // ---
  //
  if (flagShouldNotarize && platform.mac) {
    std::stringstream notarizeCommand;
    std::string username = getEnv("APPLE_ID");
    std::string password = getEnv("APPLE_ID_PASSWORD");

    notarizeCommand
      << "xcrun"
      << " altool"
      << " --notarize-app"
      << " --username \"" << username << "\""
      << " --password \"" << password << "\""
      << " --primary-bundle-id \"" << settings["bundle_identifier"] << "\""
      << " --file \"" << pathToString(pathToArchive) << "\""
    ;

    // log(notarizeCommand.str());
    auto r = exec(notarizeCommand.str().c_str());

    if (r.exitCode != 0) {
      log("Unable to notarize");
      exit(r.exitCode);
    }

    std::regex re(R"(\nRequestUUID = (.+?)\n)");
    std::smatch match;
    std::string uuid;

    if (std::regex_search(r.output, match, re)) {
      uuid = match.str(1);
    }

    int requests = 0;

    log("polling for notarization");

    while (!uuid.empty()) {
      if (++requests > 1024) {
        log("apple did not respond to the request for notarization");
        exit(1);
      }

      std::this_thread::sleep_for(std::chrono::milliseconds(1024 * 6));
      std::stringstream notarizeStatusCommand;

      notarizeStatusCommand
        << "xcrun"
        << " altool"
        << " --notarization-info " << uuid
        << " -u " << username
        << " -p " << password;

      auto r = exec(notarizeStatusCommand.str().c_str());

      std::regex re(R"(\n *Status: (.+?)\n)");
      std::smatch match;
      std::string status;

      if (std::regex_search(r.output, match, re)) {
        status = match.str(1);
      }

      if (status.find("in progress") != -1) {
        log("Checking for updates from apple");
        continue;
      }

      auto lastStatus = r.output;

      if (status.find("invalid") != -1) {
        log("apple rejected the request for notarization");

        log(lastStatus);

        std::stringstream notarizeHistoryCommand;

        notarizeHistoryCommand
          << "xcrun"
          << " altool"
          << " --notarization-history 0"
          << " -u " << username
          << " -p " << password;

        auto r = exec(notarizeHistoryCommand.str().c_str());

        if (r.exitCode != 0) {
          log("Unable to get notarization history");
          exit(r.exitCode);
        }

        log(r.output);

        exit(1);
      }

      if (status.find("success") != -1) {
        log("successfully notarized");
        break;
      }

      if (status.find("success") == -1) {
        log("apple was unable to notarize");
        break;
      }
    }

    log("finished notarization");
  }

  //
  // Windows Packaging
  // ---
  //
  if (flagShouldPackage && platform.win) {
    #ifdef _WIN32

    auto GetPackageWriter = [&](_In_ LPCWSTR outputFileName, _Outptr_ IAppxPackageWriter** writer) {
      HRESULT hr = S_OK;
      IStream* outputStream = NULL;
      IUri* hashMethod = NULL;
      APPX_PACKAGE_SETTINGS packageSettings = {0};
      IAppxFactory* appxFactory = NULL;

      hr = SHCreateStreamOnFileEx(
        outputFileName,
        STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE,
        0, // default file attributes
        TRUE, // create file if it does not exist
        NULL, // no template
        &outputStream
      );

      if (SUCCEEDED(hr)) {
        hr = CreateUri(
          L"http://www.w3.org/2001/04/xmlenc#sha256",
          Uri_CREATE_CANONICALIZE,
          0, // reserved parameter
          &hashMethod
        );
      }

      if (SUCCEEDED(hr)) {
          packageSettings.forceZip32 = TRUE;
          packageSettings.hashMethod = hashMethod;
      }

      // Create a new Appx factory
      if (SUCCEEDED(hr)) {
        hr = CoCreateInstance(
          __uuidof(AppxFactory),
          NULL,
          CLSCTX_INPROC_SERVER,
          __uuidof(IAppxFactory),
          (LPVOID*)(&appxFactory)
        );
      }

      // Create a new package writer using the factory
      if (SUCCEEDED(hr)) {
        hr = appxFactory->CreatePackageWriter(
          outputStream,
          &packageSettings,
          writer
        );
      }

      // Clean up allocated resources
      if (appxFactory != NULL) {
        appxFactory->Release();
        appxFactory = NULL;
      }

      if (hashMethod != NULL) {
        hashMethod->Release();
        hashMethod = NULL;
      }

      if (outputStream != NULL) {
        outputStream->Release();
        outputStream = NULL;
      }

      return hr;
    };


    std::wstring appx(StringToWString(pathPackage.string()) + L".appx");

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);

    if (SUCCEEDED(hr)) {
      IAppxPackageWriter* packageWriter = NULL;

      hr = GetPackageWriter(appx.c_str(), &packageWriter);

      std::function<void(LPCWSTR, fs::path)> addFiles = [&](auto basePath, auto last) {
        for (const auto & entry : fs::directory_iterator(basePath)) {
          auto p = entry.path().filename().string();

          LPWSTR mime = 0;
          FindMimeFromData(NULL, entry.path().c_str(), NULL, 0, NULL, 0, &mime, 0);

          if (p.find("AppxManifest.xml") == 0) {
            continue;
          }

          if (fs::is_directory(entry.path())) {
            addFiles(entry.path().c_str(), fs::path { last / entry.path().filename() });
            continue;
          }

          auto composite = (fs::path { last / entry.path().filename() });

          IStream* fileStream = NULL;
          hr = SHCreateStreamOnFileEx(
            entry.path().c_str(),
            STGM_READ | STGM_SHARE_EXCLUSIVE,
            0,
            FALSE,
            NULL,
            &fileStream
          );

          if (SUCCEEDED(hr)) {
            packageWriter->AddPayloadFile(
              composite.c_str(),
              mime,
              APPX_COMPRESSION_OPTION_NONE,
              fileStream
            );
          } else {
            log("Could not add file: " + entry.path().string());
          }

          if (fileStream != NULL) {
            fileStream->Release();
            fileStream = NULL;
          }
        }
      };

      addFiles(pathPackage.c_str(), fs::path {});

      IStream* manifestStream = NULL;

      if (SUCCEEDED(hr)) {
        auto p = (fs::path { pathPackage / "AppxManifest.xml" });

        hr = SHCreateStreamOnFileEx(
          p.c_str(),
          STGM_READ | STGM_SHARE_EXCLUSIVE,
          0,
          FALSE,
          NULL,
          &manifestStream
        );
      } else {
        log("Could not get package writer or add files");
      }

      if (SUCCEEDED(hr)) {
        hr = packageWriter->Close(manifestStream);
      } else {
        log("Could not generate AppxManifest.xml");
      }

      if (manifestStream != NULL) {
        manifestStream->Release();
        manifestStream = NULL;
      }
      if (packageWriter != NULL) {
        packageWriter->Release();
        packageWriter = NULL;
      }

      CoUninitialize();
    } else {
      log("Unable to initialize package writer");
    }

    if (SUCCEEDED(hr)) {
      log("Package saved");
    }
    else {
      _com_error err(hr);
      std::string msg = std::string( err.ErrorMessage() );
      log("Unable to save package; " + msg);
    }

    #endif
  }


  //
  // Windows Code Signing
  //
  if (flagCodeSign && platform.win) {
    //
    // https://www.digicert.com/kb/code-signing/signcode-signtool-command-line.htm
    //
    auto pathToSignTool = getEnv("SIGNTOOL");

    if (pathToSignTool.size() == 0) {
      log("missing env var SIGNTOOL, should be the path to the Windows SDK signtool.exe binary.");
      exit(1);
    }

    std::stringstream signCommand;
    std::string password = getEnv("CSC_KEY_PASSWORD");

    signCommand
      << "\"" << pathToSignTool << "\""
      << " sign"
      << " /debug"
      << " /tr http://timestamp.digicert.com"
      << " /td sha256"
      << " /fd sha256"
      << " /f cert.pfx"
      << " /p " << password
      << " "
      << pathToString(pathPackage) << ".appx"
      << " 2>&1 | tee sign.log";

      auto r = exec(signCommand.str().c_str());

      if (r.exitCode != 0) {
        log("Unable to sign");

        log("---");
        log(pathPackage.string());
        log("---");

        log("---");
        log(r.output);
        log("---");

        exit(r.exitCode);
      }
  }

  if (flagShouldRun) {
    std::string execName = "";
    if (platform.win) {
      execName = (settings["executable"] + ".exe");
    } else {
      execName = (settings["executable"]);
    }

    auto cmd = pathToString(fs::path {
      pathBin /
      execName
    });

    std::system(cmd.c_str());
  }

  return 0;
}
