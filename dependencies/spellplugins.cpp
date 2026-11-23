// Explicit imports for our private static Sonnet backends. No plugin search
// paths, KDE installation, bundled dictionaries, or runtime downloads required.
#include <QtPlugin>
#if defined(Q_OS_MACOS)
Q_IMPORT_PLUGIN(NSSpellCheckerClient)
#elif defined(Q_OS_WIN)
Q_IMPORT_PLUGIN(ISpellCheckerClient)
#elif defined(Q_OS_LINUX)
Q_IMPORT_PLUGIN(HunspellClient)
#endif
