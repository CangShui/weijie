# Image groups

Subfolders are groups. `default` is displayed as “默认”; pictures directly
under `images` are displayed as “未分类”. Pictures and groups can be deleted.
Opening the picker or changing the directory refreshes the displayed groups.

## Default assets

These 96×96 transparent PNGs are generated from the color layers
(`COLR`/`CPAL`) of the system **Segoe UI Emoji** font. The emoji list follows
SnapCut's `EmojiStickerCatalog`.

They are loaded by `cpp/src/emoji_renderer.cpp` for the screenshot emoji
picker and annotation renderer.
