/*
 * image.h — save and render model-generated images.
 *
 * Image-output models return pictures in the reply as data: URLs
 * (data:<mime>;base64,<payload>). This module base64-decodes them to
 * files and renders them inline by shelling out to `chafa`, which handles
 * every image format and the terminal's graphics protocol (Kitty on
 * kitty/Ghostty/WezTerm, Sixel, iTerm2, or symbol art) itself. chafa is
 * an optional runtime helper — like the clipboard tools — so callers fall
 * back to showing the saved path when it is absent. orc's text pipeline
 * is unaffected; this only handles the image parts the API returns.
 */
#ifndef ORC_IMAGE_H
#define ORC_IMAGE_H

/*
 * Decode a data: URL and write the bytes to
 * <dir>/img-<ts>-<index>.<ext>, the extension taken from the URL's mime
 * subtype (png, jpeg, …). Returns a malloc'd path (caller frees) or NULL
 * if the URL is not a base64 data URL or the write failed. `dir` must
 * already exist.
 */
char *img_save_data_url(const char *data_url, const char *dir,
                        long ts, int index);

/*
 * Render the image file at `path` inline via chafa. Returns true if it
 * was displayed; false when stdout is not a terminal, chafa is not
 * installed, the path is unsafe to pass to the shell, or chafa failed —
 * so the caller can fall back to printing the path.
 */
bool img_render_file(const char *path);

/*
 * Render every image referenced by an "[image: <path>]" marker in `text`
 * (as written into persisted assistant messages), each via
 * img_render_file. Used to re-display images when a conversation is
 * resumed. Missing/unrenderable files are simply skipped.
 */
void img_render_markers(const char *text);

#endif /* ORC_IMAGE_H */
