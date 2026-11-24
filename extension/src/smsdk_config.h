/* Copyright 2026 ETF2L Contributors, Dave
 *
 * The extension's description for SourceMod's smsdk_ext. */
#ifndef _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
#define _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_

#define SMEXT_CONF_NAME        "Git"
#define SMEXT_CONF_DESCRIPTION "Clones and fetches git repositories over HTTPS and reads them for plugins"
/* The version and the day it was set. A fixed date rather than __DATE__,
 * which would make every build differ; change both together. */
#define SMEXT_CONF_VERSION     "1.0.0"
#define SMEXT_CONF_DATESTRING  "2026-11-20"
#define SMEXT_CONF_AUTHOR      "ETF2L Contributors, Dave"
#define SMEXT_CONF_URL         "https://github.com/etf2l/sm-git"
#define SMEXT_CONF_LOGTAG      "GIT"
#define SMEXT_CONF_LICENSE     "GPL"

#define SMEXT_LINK(name) SDKExtension *g_pExtensionIface = name;

#define SMEXT_ENABLE_FORWARDSYS
#define SMEXT_ENABLE_HANDLESYS
#define SMEXT_ENABLE_PLUGINSYS
#define SMEXT_ENABLE_TEXTPARSERS

#endif // _INCLUDE_SOURCEMOD_EXTENSION_CONFIG_H_
