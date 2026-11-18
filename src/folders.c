/*
 * The user's folders and the app's data folder. On Linux and the other XDG systems gpuic looks itself (on either platform, so the
 * native Wayland one needs no SDL): ~/.config/user-dirs.dirs (XDG_CONFIG_HOME), else the default English name under $HOME, which
 * SDL does not fall back to; elsewhere SDL answers. Strings are kept by the app until it ends.
 */
#include "internal.h"

#ifdef GP_XDG_FOLDERS
/* XDG_<key>_DIR from user-dirs.dirs: "$HOME/Documents" or "/abs/path", with \ escaping. NULL if not set there. */
static char* xdg_lookup(const char* home, const char* key)
{
    char file[4096], want[64], line[4096];
    const char* config = getenv("XDG_CONFIG_HOME");
    if(config && *config) {
        snprintf(file, sizeof file, "%s/user-dirs.dirs", config);
    } else {
        snprintf(file, sizeof file, "%s/.config/user-dirs.dirs", home);
    }
    FILE* f = fopen(file, "r");
    if(!f) {
        return NULL;
    }
    int n = snprintf(want, sizeof want, "XDG_%s_DIR=\"", key);
    char* found = NULL;
    while(fgets(line, sizeof line, f)) {
        const char* p = line + strspn(line, " \t");
        if(strncmp(p, want, (size_t)n) != 0) {
            continue;
        }
        p += n;
        char out[4096];
        size_t o = 0;
        if(strncmp(p, "$HOME", 5) == 0 && (p[5] == '/' || p[5] == '"')) {
            o = (size_t)snprintf(out, sizeof out, "%s", home);
            p += 5;
        } else if(*p != '/') {
            continue; /* (only absolute paths, or $HOME-relative ones, are valid) */
        }
        for(; *p && *p != '"' && o + 1 < sizeof out; p++) {
            if(*p == '\\' && p[1]) {
                p++;
            }
            out[o++] = *p;
        }
        out[o] = 0;
        free(found);
        found = strdup(out); /* (the last one wins, as in xdg-user-dir-lookup) */
    }
    fclose(f);
    return found;
}

static char* user_folder(int which)
{
    static const char* const keys[][2] = {
        [GP_FOLDER_DESKTOP] = {     "DESKTOP",   "Desktop" },
        [GP_FOLDER_DOCUMENTS] = {   "DOCUMENTS", "Documents" },
        [GP_FOLDER_DOWNLOADS] = {    "DOWNLOAD", "Downloads" },
        [GP_FOLDER_MUSIC] = {       "MUSIC",     "Music" },
        [GP_FOLDER_PICTURES] = {    "PICTURES",  "Pictures" },
        [GP_FOLDER_PUBLICSHARE] = { "PUBLICSHARE",    "Public" },
        [GP_FOLDER_TEMPLATES] = {   "TEMPLATES", "Templates" },
        [GP_FOLDER_VIDEOS] = {      "VIDEOS",    "Videos" },
    };
    const char* home = getenv("HOME");
    if(!home || !*home) {
        return NULL;
    }
    if(which == GP_FOLDER_HOME) {
        return strdup(home);
    }
    if(!keys[which][0]) {
        return NULL; /* saved games, screenshots: XDG has no such folder */
    }
    char* dir = xdg_lookup(home, keys[which][0]);
    if(!dir) {
        size_t len = strlen(home) + strlen(keys[which][1]) + 2;
        dir = malloc(len);
        snprintf(dir, len, "%s/%s", home, keys[which][1]);
    }
    return dir;
}

static char* data_folder(const char* app_name)
{
    const char* base = getenv("XDG_DATA_HOME");
    const char* home = getenv("HOME");
    char dir[4096];
    if(base && *base) {
        snprintf(dir, sizeof dir, "%s/%s", base, app_name);
    } else if(home && *home) {
        snprintf(dir, sizeof dir, "%s/.local/share/%s", home, app_name);
    } else {
        return NULL;
    }
    return gp_platform_mkdirs(dir) ? strdup(dir) : NULL;
}
#else
# define user_folder gp_platform_user_folder
# define data_folder gp_platform_data_folder
#endif

static const char* keep(gp_app* app, char* s)
{
    if(!s) {
        return NULL;
    }
    size_t n = strlen(s);
    while(n > 1 && (s[n - 1] == '/' || s[n - 1] == '\\')) { /* (SDL's end in a separator) */
        s[--n] = 0;
    }
    for(size_t i = 0; i < app->nfolders; i++) {
        if(strcmp(app->folders[i], s) == 0) {
            free(s);
            return app->folders[i];
        }
    }
    app->folders = realloc(app->folders, (app->nfolders + 1) * sizeof *app->folders);
    app->folders[app->nfolders++] = s;
    return s;
}

const char* gp_user_folder(gp_app* app, gp_folder which)
{
    if((int)which < 0 || which >= GP_FOLDER_COUNT) {
        return NULL;
    }
    return keep(app, user_folder(which));
}

const char* gp_app_data_folder(gp_app* app, const char* app_name)
{
    if(!app_name || !*app_name) {
        return NULL;
    }
    return keep(app, data_folder(app_name));
}

void gp_folders_free(gp_app* app)
{
    for(size_t i = 0; i < app->nfolders; i++) {
        free(app->folders[i]);
    }
    free(app->folders);
}
