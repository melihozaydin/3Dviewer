FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    g++ \
    libglfw3-dev \
    libglew-dev \
    libglm-dev \
    libxinerama-dev \
    libxcursor-dev \
    libxi-dev \
    libtiff-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY scan_viewer.cpp .
COPY imgui /app/imgui

RUN g++ -o scan_viewer scan_viewer.cpp \
    imgui/imgui.cpp \
    imgui/imgui_draw.cpp \
    imgui/imgui_widgets.cpp \
    imgui/imgui_tables.cpp \
    imgui/backends/imgui_impl_glfw.cpp \
    imgui/backends/imgui_impl_opengl3.cpp \
    -Iimgui -Iimgui/backends \
    -lGL -lGLEW -lglfw -ldl -pthread -ltiff