# The version, and the commit the binary was built from, for About to name.
# Shared by the app and the tests so the two cannot drift: a test that asserts
# the version has to be built with the same definition of it.
# Upstream's v0.5.0 is this fork's base -- its tag is our merge base -- plus
# the macOS work on top, so the version says both. The .pro said 0.1.0, which
# was never true of this code.
VERSION = 0.5.0-macos.3

OMAWRITE_COMMIT = $$system(git -C $$PWD rev-parse --short HEAD 2>/dev/null)
isEmpty(OMAWRITE_COMMIT): OMAWRITE_COMMIT = unknown
else {
    OMAWRITE_DIRTY = $$system(git -C $$PWD status --porcelain 2>/dev/null)
    !isEmpty(OMAWRITE_DIRTY): OMAWRITE_COMMIT = $${OMAWRITE_COMMIT}+
}

DEFINES += OMAWRITE_VERSION=\\\"$$VERSION\\\"
DEFINES += OMAWRITE_COMMIT=\\\"$$OMAWRITE_COMMIT\\\"

# Where that commit can be read. Taken from the remote so a fork points at
# itself, and normalised from SSH to https so it opens in a browser.
OMAWRITE_REMOTE = $$system(git -C $$PWD remote get-url origin 2>/dev/null)
OMAWRITE_REMOTE = $$replace(OMAWRITE_REMOTE, ^git@github\\.com:, https://github.com/)
OMAWRITE_REMOTE = $$replace(OMAWRITE_REMOTE, \\.git$, )
DEFINES += OMAWRITE_REMOTE=\\\"$$OMAWRITE_REMOTE\\\"
