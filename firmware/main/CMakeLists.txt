idf_component_register(
    SRCS "main.cpp"
         "../components/ui/review_ui.cpp"
         "fonts/font_cjk_16.c"
         "fonts/font_cjk_20.c"
         "fonts/font_cjk_28.c"
         "fonts/font_cjk_48.c"
    INCLUDE_DIRS "."
                 "../components/fsrs/include"
                 "../components/deck/include"
                 "../components/session/include"
                 "../components/persist/include"
                 "../components/ui/include"
    EMBED_FILES "../../decks/hsk1-2.srs"
    REQUIRES devctl littlefs
)
