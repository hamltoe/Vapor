#ifndef VAPOR_INSTALLER_H
#define VAPOR_INSTALLER_H

#include <stddef.h>

/* Families we can drive without per-title logic. Detection uses the file
 * name, sibling files, and a short signature scan — never a game id. */
typedef enum {
    VAPOR_INST_NONE = 0,
    VAPOR_INST_INNO,
    VAPOR_INST_NSIS,
    VAPOR_INST_MSI,
    VAPOR_INST_ISHIELD,
    VAPOR_INST_EXE,
    VAPOR_INST_ISO
} vapor_inst_kind;

const char *vapor_inst_kind_name(vapor_inst_kind kind);

vapor_inst_kind vapor_inst_detect(const char *exec, const char *cwd);

/* Write unattended arguments that install into `dest`. 0 if `out` is filled,
 * 1 if this family has no silent mode (run the file with no extra args). */
int vapor_inst_silent_params(vapor_inst_kind kind, const char *exec,
                             const char *dest, char *out, size_t outsz);

/* 1 if this PE uses disc copy-protection Windows 10+ cannot load
 * (SafeDisc `BoG_` cookie, or SECDRV.SYS sitting next to the exe). */
int vapor_exe_is_copy_protected(const char *path);

/* Search `dir` for a .exe that is not copy-protected. Prefers names that
 * slug-match `id`. 0 if `out` is filled. */
int vapor_find_unprotected_exe(const char *dir, const char *id, char *out,
                               size_t outsz);

#endif /* VAPOR_INSTALLER_H */
