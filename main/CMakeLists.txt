# 设置变量APP_SRC，包含driver/src/目录下的所有.cpp和.c文件
file(GLOB APP_SRC "driver/src/*.cpp" "driver/src/*.c" "utils/src/*.c" "utils/src/*.cpp" "main.cpp")
set(APP_INC "." "driver/inc" "utils/inc")

idf_component_register(SRCS ${APP_SRC}
                       INCLUDE_DIRS ${APP_INC})
