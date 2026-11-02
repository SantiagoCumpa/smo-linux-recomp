/* smo_guide.c - the console's offline web applet (the game's Help, its Action Guide).
 *
 * On the console nn::web::ShowOfflineHtmlPage starts the browser applet, which shows the game's HTML manual over the game,
 * and the calling thread waits until the player closes it. The Windows build showed the page inside the game window with
 * Microsoft Edge WebView2. The Linux build has no embedded browser: guide_show answers 0 and nnapplet.c opens the page
 * (a copy with the console browser's window.nx script and a <base> at the page's folder, in build/tmp) in the desktop's
 * web browser through xdg-open, then answers as if the player had closed it.
 *
 * Accepted difference (PORT_TO_LINUX.md): the game is not held while the page is open in the browser, and a callback
 * address the page goes to (the Action Guide's "change mode" item, http://localhost/...) does not reach the game. */
#include <stddef.h>

/* 1: shown in the game window and closed by the player; 2: the page went to a callback address (copied to callback);
 * 0: not shown here (nnapplet.c then opens it in the web browser). */
int guide_show(const char *url, const char *script, char *callback, size_t cap) {
    (void)url; (void)script; (void)callback; (void)cap;
    return 0;
}
