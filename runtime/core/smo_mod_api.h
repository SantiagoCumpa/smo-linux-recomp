/* smo_mod_api.h - what a code mod sees of the runtime (runtime/core/mods.c loads it).
 *
 * A code mod is C compiled into a shared library of its own: the C files of defaults/mods/<id>/src, built by
 * tools/lifter/build_mods.py into mods/<id>/<library> (named by `library =` in its mod.ini). When the mod is on, the
 * runtime loads it after the game's code is loaded and before the game runs, and calls
 *
 *     int smo_mod_init(const SmoModApi *api);      0 = installed; anything else = not installed (said in the log)
 *     void smo_mod_report(FILE *out);              optional: lines for the log when the game ends
 *
 * The game's own code is the lifted code in libsmo_game.so: one host function per guest function (void f(Ctx *)),
 * found by its guest address (the GAME_SYM_* / GAME_FN_* addresses of build/gen/image.h, which a mod may include: it
 * is built against this game's build). Guest memory is mapped at its own addresses (runtime.h rd32 / wr32 ...). A mod
 * changes the game by pointing calls of the lifted code at its own functions (see fast-sampler-sort) or by what it
 * does when called. */
#ifndef SMO_MOD_API_H
#define SMO_MOD_API_H
#include <stdint.h>
#include <stdio.h>
#include "runtime.h"

#define SMO_MOD_API_VERSION 1
typedef void (*SmoGameFn)(Ctx *);
typedef struct SmoModApi {
    int version;                                   /* SMO_MOD_API_VERSION of the runtime that loaded the mod */
    const char *mod_dir;                           /* the mod's folder (mods/<id>) */
    const char *mod_id;                            /* <id>: the folder's name, as config/mods.ini lists it */
    SmoGameFn (*game_fn)(uint64_t guest);          /* the lifted function whose guest entry is `guest`, NULL when none */
    uintptr_t (*game_fn_end)(SmoGameFn fn);        /* the end of that function's host code (the next one's start), 0 if unknown */
    void (*trap)(const char *what, uint64_t guest_addr);      /* stop the game with a report (a mod's own check failed) */
    int (*feature_on)(const char *feature);        /* 1 when a mod that is on declares that feature (mod.ini feature =) */
} SmoModApi;

typedef int (*SmoModInit)(const SmoModApi *api);
typedef void (*SmoModReport)(FILE *out);
#endif
