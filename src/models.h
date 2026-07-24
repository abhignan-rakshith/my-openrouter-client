/*
 * models.h — interactive model picker.
 *
 * Fetches the list of text-output models from OpenRouter (the only kind
 * orc can render), parses it with the depth-aware array iterator in
 * jsonutil, and presents a live-filtering picker on the alternate screen
 * so the active model can be switched mid-session. Favourites are starred
 * and persisted in the user config, floating to the top on future opens.
 */
#ifndef ORC_MODELS_H
#define ORC_MODELS_H

/*
 * Run the interactive model picker. Fetches and parses the model list,
 * then lets the user filter (by typing), star/unstar (Tab, persisted),
 * choose (Enter), or cancel (Esc). `current` (may be NULL) is the active
 * model, highlighted in the list.
 *
 * Returns the malloc'd id of the chosen model (the caller frees it), or
 * NULL if cancelled, unchanged, on error, or when stdin/stdout is not a
 * terminal. A diagnostic is printed on fetch/parse failure.
 *
 * When a model is chosen and `chosen_outputs_images` is non-NULL, it is
 * set to whether that model can emit image output (which orc renders as
 * text only), so the caller can warn. It is left untouched when NULL is
 * returned.
 */
char *models_pick(const char *api_key, const char *current,
                  bool *chosen_outputs_images);

#endif /* ORC_MODELS_H */
