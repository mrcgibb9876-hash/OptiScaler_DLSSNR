//{{NO_DEPENDENCIES}}
// Microsoft Visual C++ generated include file.
// Used by OptiScaler.rc
//
#ifdef _DEBUG
#define VER_BUILD_DATE "Debug Build"
#define VER_BUILD_COMMIT "Debug"
#else
#include "resource_build_date.h"
#include "resource_build_commit.h"
#endif // !_DEBUG

#define VS_VERSION_INFO 1

// Next default values for new objects
//
#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE 101
#define _APS_NEXT_COMMAND_VALUE 40001
#define _APS_NEXT_CONTROL_VALUE 1001
#define _APS_NEXT_SYMED_VALUE 101
#endif
#endif

#define STRINGIZE_(s) #s
#define STRINGIZE(s) STRINGIZE_(s)

// This fork's own version, and it has to match the tag its releases are cut from, because
// version_check.cpp compares the two: remoteVersion comes from the latest release tag, local
// comes from these three numbers. They were 10.0.0 -- upstream OptiScaler's numbering, inherited
// with the fork -- while this fork tagged its releases v1.0.x. So the comparison was permanently
// 1.0.41 > 10.0.0 == false and "a new release is available" could never fire, whatever was
// published. Bringing both to 2.0.0 lines them up and the check works again.
#define VER_MAJOR_VERSION 2
#define VER_MINOR_VERSION 1
#define VER_HOTFIX_VERSION 0
#define VER_BUILD_NUMBER 1

// A tagged release is not a dev build: leaving VER_DEV_RELEASE on labelled every shipped DLL
// "2.0.0-dev" in its own version resource and in the panel.
// #define VER_DEV_RELEASE
// #define VER_PRE_RELEASE

#define VER_FILE_VERSION VER_MAJOR_VERSION, VER_MINOR_VERSION, VER_HOTFIX_VERSION, VER_BUILD_NUMBER
#define VER_FILE_VERSION_STR                                                                                           \
    STRINGIZE(VER_MAJOR_VERSION) "." STRINGIZE(VER_MINOR_VERSION) "." STRINGIZE(VER_HOTFIX_VERSION) "." STRINGIZE(VER_BUILD_NUMBER)
#define OPTI_VERSION STRINGIZE(VER_MAJOR_VERSION) "." STRINGIZE(VER_MINOR_VERSION) "." STRINGIZE(VER_HOTFIX_VERSION)

#define VER_PRODUCT_VERSION VER_FILE_VERSION

#ifdef VER_DEV_RELEASE
#define VER_PRODUCT_VERSION_STR                                                                                        \
    STRINGIZE(VER_MAJOR_VERSION) "." STRINGIZE(VER_MINOR_VERSION) "." STRINGIZE(VER_HOTFIX_VERSION) "-dev (" VER_BUILD_COMMIT ") (" VER_BUILD_DATE ")"
#elif VER_PRE_RELEASE
#define VER_PRODUCT_VERSION_STR                                                                                        \
    STRINGIZE(VER_MAJOR_VERSION) "." STRINGIZE(VER_MINOR_VERSION) "." STRINGIZE(VER_HOTFIX_VERSION) "-pre" STRINGIZE(VER_BUILD_NUMBER) " (" VER_BUILD_COMMIT ") (" VER_BUILD_DATE ")"
#else
#define VER_PRODUCT_VERSION_STR                                                                                        \
    STRINGIZE(VER_MAJOR_VERSION) "." STRINGIZE(VER_MINOR_VERSION) "." STRINGIZE(VER_HOTFIX_VERSION) "-final (" VER_BUILD_COMMIT ")"
#endif // VER_PRE_RELEASE

#define VER_PRODUCT_NAME "OptiScaler v" VER_PRODUCT_VERSION_STR
