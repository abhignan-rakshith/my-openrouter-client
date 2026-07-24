/*
 * clipboard.h — system clipboard image access.
 *
 * Pulls an image off the OS clipboard (macOS via osascript; Linux/BSD via
 * a wl-paste or xclip helper chosen at runtime) and turns saved images
 * into the base64 data URLs the OpenRouter multimodal message format
 * expects. No extra link-time dependencies — the helpers are invoked as
 * subprocesses.
 */
#ifndef ORC_CLIPBOARD_H
#define ORC_CLIPBOARD_H

/*
 * If the clipboard currently holds an image, write it to `path` as PNG.
 * Returns 0 on success, 1 if the clipboard has no image (or it could
 * not be converted), -1 on internal error — including, on Linux/BSD, no
 * display session or no clipboard helper (wl-paste/xclip) installed.
 */
int clip_image_save(const char *path);

/*
 * Read a PNG file and return it as a malloc'd
 * "data:image/png;base64,..." string, or NULL on error/OOM.
 */
char *clip_file_data_url(const char *path);

#endif /* ORC_CLIPBOARD_H */
