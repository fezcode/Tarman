#ifndef TARMAN_VERSION_H
#define TARMAN_VERSION_H

/* The one place the version is written down. main.c prints it for --version
 * and in the About panel; CMakeLists.txt parses it at configure time so the
 * Windows VERSIONINFO resource cannot drift from the running app. */
#define TARMAN_VERSION "0.1.0"

#endif /* TARMAN_VERSION_H */
