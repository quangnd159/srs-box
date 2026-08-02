idf_component_register(
    SRCS "power.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc
             driver
)
