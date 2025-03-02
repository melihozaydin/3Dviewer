#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <vector>
#include <iostream>
#include <string>
#include <tiffio.h>
#include <dirent.h>
#include <cstring>
#include <fftw3.h>
#include <algorithm>
#include <limits>

struct Point3D {
    float x, y, z;
};

class ScanViewer {
private:
    GLFWwindow* window;
    std::vector<Point3D> points;
    std::vector<float> zMap;       // Current filtered Z map
    std::vector<float> inputZMap;  // Original input Z map in normalized range
    std::vector<float> rawZMap;    // Raw Z values from TIFF
    std::vector<float> filterParams;
    glm::mat4 projection, view, model;
    float zoom = 5.0f;
    float rotX = 0.0f, rotY = 0.0f;
    float panX = 0.0f, panY = 0.0f;
    float zMin = 0.0f, zMax = 0.0f;  // Use raw Z range for coloring
    float zScale = 1.0f;
    float filterLowCutoff = 0.0f, filterHighCutoff = 0.0f;  // Bandpass cutoffs in micrometers
    GLuint shaderProgram, vao, vbo;
    bool mouseDragging = false;
    bool panning = false;
    double lastX = 0.0, lastY = 0.0;
    std::string currentDataSource = "Generated sample data";
    char folderPathBuffer[256] = "";
    std::vector<std::string> tiffFiles;
    std::string errorMessage;
    int colorLUT = 0;  // 0: Jet, 1: Viridis, 2: Plasma, 3: Hot, 4: Cool, 5: Turbo
    uint32_t width = 0, height = 0;
    float inputZMin = -1.0f, inputZMax = 1.0f;  // Normalized range
    float rawZMin = 0.0f, rawZMax = 0.0f;      // Original range
    bool histogramNeedsUpdate = true;

    const char* vertexShaderSource = R"(
        #version 130
        attribute vec3 aPos;
        uniform mat4 mvp;
        uniform float zMin;
        uniform float zMax;
        uniform float zScale;
        uniform int colorLUT;
        varying vec3 vColor;
        void main() {
            vec3 scaledPos = vec3(aPos.x, aPos.y, aPos.z * zScale);
            gl_Position = mvp * vec4(scaledPos, 1.0);
            float t = clamp((aPos.z - zMin) / (zMax - zMin), 0.0, 1.0);
            if (colorLUT == 0) {  // Jet (more gradations)
                if (t < 0.25) vColor = mix(vec3(0,0,1), vec3(0,0.5,1), t/0.25);
                else if (t < 0.5) vColor = mix(vec3(0,0.5,1), vec3(0,1,1), (t-0.25)/0.25);
                else if (t < 0.75) vColor = mix(vec3(0,1,1), vec3(1,1,0), (t-0.5)/0.25);
                else vColor = mix(vec3(1,1,0), vec3(1,0,0), (t-0.75)/0.25);
            } else if (colorLUT == 1) {  // Viridis (more gradations)
                if (t < 0.33) vColor = mix(vec3(0.267,0.676,0.997), vec3(0.043,0.141,0.278), t/0.33);
                else if (t < 0.66) vColor = mix(vec3(0.043,0.141,0.278), vec3(0.5,0.5,0.5), (t-0.33)/0.33);
                else vColor = mix(vec3(0.5,0.5,0.5), vec3(1,0.9,0), (t-0.66)/0.34);
            } else if (colorLUT == 2) {  // Plasma
                vColor = vec3(0.908*t + 0.051, 0.463*t + 0.281, 0.996*t + 0.133);
            } else if (colorLUT == 3) {  // Hot
                vColor = vec3(3.0*t, t > 0.33 ? 3.0*(t-0.33) : 0.0, t > 0.66 ? 3.0*(t-0.66) : 0.0);
            } else if (colorLUT == 4) {  // Cool
                vColor = vec3(t, 1.0-t, 1.0);
            } else {  // Turbo
                float r = 0.1357 + t * (4.5970 - t * (42.8537 - t * (151.0138 - t * (218.7175 - t * 115.2778))));
                float g = 0.0914 + t * (2.1855 + t * (4.2596 - t * (71.3487 - t * (206.5138 - t * 165.4033))));
                float b = 0.1066 + t * (5.9399 - t * (49.9290 - t * (171.2617 - t * (258.5662 - t * 136.5015))));
                vColor = vec3(r, g, b);
            }
            vColor = clamp(vColor, 0.0, 1.0);
        }
    )";

    const char* fragmentShaderSource = R"(
        #version 130
        varying vec3 vColor;
        void main() {
            gl_FragColor = vec4(vColor, 1.0);
        }
    )";

    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
        ScanViewer* viewer = static_cast<ScanViewer*>(glfwGetWindowUserPointer(window));
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !ImGui::GetIO().WantCaptureMouse) {
            if (mods & GLFW_MOD_SHIFT) {
                viewer->panning = true;
            } else {
                viewer->mouseDragging = true;
            }
            glfwGetCursorPos(window, &viewer->lastX, &viewer->lastY);
        } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
            viewer->mouseDragging = false;
            viewer->panning = false;
        }
    }

    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
        ScanViewer* viewer = static_cast<ScanViewer*>(glfwGetWindowUserPointer(window));
        if (!ImGui::GetIO().WantCaptureMouse) {
            double dx = xpos - viewer->lastX;
            double dy = ypos - viewer->lastY;
            if (viewer->mouseDragging) {
                viewer->rotY += static_cast<float>(dx * 0.2);
                viewer->rotX += static_cast<float>(dy * 0.2);
            } else if (viewer->panning) {
                viewer->panX += static_cast<float>(dx * 0.01);
                viewer->panY -= static_cast<float>(dy * 0.01);
            }
            viewer->lastX = xpos;
            viewer->lastY = ypos;
        }
    }

    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
        ScanViewer* viewer = static_cast<ScanViewer*>(glfwGetWindowUserPointer(window));
        if (!ImGui::GetIO().WantCaptureMouse) {
            viewer->zoom -= static_cast<float>(yoffset * 0.5);
            viewer->zoom = glm::clamp(viewer->zoom, 1.0f, 20.0f);
        }
    }

    bool loadTiffZMap(const std::string& path) {
        TIFF* tif = TIFFOpen(path.c_str(), "r");
        if (!tif) {
            errorMessage = "Failed to open TIFF file: " + path;
            return false;
        }

        uint32_t w, h;
        uint16_t bitsPerSample, sampleFormat, samplesPerPixel;
        if (!TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w) || 
            !TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h) ||
            !TIFFGetField(tif, TIFFTAG_BITSPERSAMPLE, &bitsPerSample) ||
            !TIFFGetField(tif, TIFFTAG_SAMPLEFORMAT, &sampleFormat) ||
            !TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &samplesPerPixel)) {
            errorMessage = "Failed to read TIFF metadata: " + path;
            TIFFClose(tif);
            return false;
        }

        if (samplesPerPixel != 1) {
            errorMessage = "Only single-channel TIFFs are supported: " + path;
            TIFFClose(tif);
            return false;
        }

        width = w;
        height = h;
        points.clear();
        zMap.resize(width * height);
        inputZMap.resize(width * height);
        rawZMap.resize(width * height);
        float xScale = 10.0f / width;
        float yScale = 10.0f / height;
        std::vector<char> scanline(TIFFScanlineSize(tif));

        float zMinVal = std::numeric_limits<float>::max();
        float zMaxVal = std::numeric_limits<float>::lowest();

        for (uint32_t y = 0; y < height; y++) {
            if (TIFFReadScanline(tif, scanline.data(), y) < 0) {
                errorMessage = "Failed to read scanline " + std::to_string(y) + " from: " + path;
                TIFFClose(tif);
                return false;
            }
            for (uint32_t x = 0; x < width; x++) {
                float z;
                if (bitsPerSample == 32 && sampleFormat == SAMPLEFORMAT_IEEEFP) {
                    z = reinterpret_cast<float*>(scanline.data())[x];  // Raw μm
                } else if (bitsPerSample == 16 && sampleFormat == SAMPLEFORMAT_UINT) {
                    z = (reinterpret_cast<uint16_t*>(scanline.data())[x] / 65535.0f) * 2.0f - 1.0f;
                } else if (bitsPerSample == 8 && sampleFormat == SAMPLEFORMAT_UINT) {
                    z = (reinterpret_cast<uint8_t*>(scanline.data())[x] / 255.0f) * 2.0f - 1.0f;
                } else {
                    errorMessage = "Unsupported TIFF format (" + 
                                  std::to_string(bitsPerSample) + " bits, format " + 
                                  std::to_string(sampleFormat) + "): " + path;
                    TIFFClose(tif);
                    return false;
                }
                int idx = y * width + x;
                rawZMap[idx] = z;
                zMinVal = std::min(zMinVal, z);
                zMaxVal = std::max(zMaxVal, z);
            }
        }

        // Normalize Z to -1 to 1 range for rendering only
        float zRange = zMaxVal - zMinVal;
        if (zRange > 0) {
            for (size_t i = 0; i < rawZMap.size(); i++) {
                inputZMap[i] = 2.0f * (rawZMap[i] - zMinVal) / zRange - 1.0f;
                zMap[i] = rawZMap[i];  // Start with raw values for filtering
            }
        } else {
            inputZMap = rawZMap;
            zMap = rawZMap;
        }

        points.clear();
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                int idx = y * width + x;
                float xPos = (x - width/2.0f) * xScale;
                float yPos = (y - height/2.0f) * yScale;
                points.push_back({xPos, yPos, rawZMap[idx]});
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(Point3D), points.data(), GL_STATIC_DRAW);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error after loading TIFF: " << err << std::endl;
        }

        TIFFClose(tif);
        currentDataSource = path;
        inputZMin = -1.0f;
        inputZMax = 1.0f;
        rawZMin = zMinVal;
        rawZMax = zMaxVal;
        zMin = rawZMin;
        zMax = rawZMax;
        filterLowCutoff = rawZMin;
        filterHighCutoff = rawZMax;
        errorMessage.clear();
        std::cout << "Loaded TIFF: " << width << "x" << height << " (" << points.size() << " points), Raw Z range: " << zMinVal << " to " << zMaxVal << std::endl;
        return true;
    }

    void loadDefaultData() {
        width = 101;
        height = 101;
        points.clear();
        zMap.resize(width * height);
        inputZMap.resize(width * height);
        rawZMap.resize(width * height);
        for (int i = -50; i <= 50; i++) {
            for (int j = -50; j <= 50; j++) {
                float x = i * 0.1f;
                float y = j * 0.1f;
                float z = sin(x) * cos(y) + sin(sqrt(x*x + y*y)) * 0.5f;
                int idx = (i + 50) * width + (j + 50);
                rawZMap[idx] = z;
                inputZMap[idx] = z;  
                zMap[idx] = z;
                points.push_back({x, y, z});
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(Point3D), points.data(), GL_STATIC_DRAW);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error after loading default data: " << err << std::endl;
        }
        currentDataSource = "Generated sample data";
        inputZMin = -1.0f;
        inputZMax = 1.0f;
        rawZMin = -1.0f;
        rawZMax = 1.0f;
        zMin = rawZMin;
        zMax = rawZMax;
        filterLowCutoff = rawZMin;
        filterHighCutoff = rawZMax;
        errorMessage.clear();
    }

    void applyFourierFilter() {
        if (rawZMap.empty() || width == 0 || height == 0) return;

        fftw_complex* in = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * width * height);
        fftw_complex* out = (fftw_complex*)fftw_malloc(sizeof(fftw_complex) * width * height);
        fftw_plan forward = fftw_plan_dft_2d(height, width, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
        fftw_plan inverse = fftw_plan_dft_2d(height, width, out, in, FFTW_BACKWARD, FFTW_ESTIMATE);

        for (size_t i = 0; i < rawZMap.size(); i++) {
            in[i][0] = rawZMap[i];
            in[i][1] = 0.0;
        }

        fftw_execute(forward);

        // Use spatial frequency based on physical size
        float pixelSize = 10.0f / std::max(width, height);  // Assuming 10 units across the image
        float filterLowFreq = filterLowCutoff / (pixelSize * 1e6f);  // Convert μm to cycles/unit
        float filterHighFreq = filterHighCutoff / (pixelSize * 1e6f);  // Convert μm to cycles/unit
        if (filterLowFreq > filterHighFreq) std::swap(filterLowFreq, filterHighFreq);

        float nyquist = 0.5f;  // Nyquist frequency (max freq = 0.5 cycles/pixel)
        float lowCutoffFreq = filterLowFreq * pixelSize;  // Scale to normalized frequency
        float highCutoffFreq = filterHighFreq * pixelSize;
        lowCutoffFreq = std::max(0.0f, std::min(lowCutoffFreq, nyquist));
        highCutoffFreq = std::max(0.0f, std::min(highCutoffFreq, nyquist));

        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                int idx = y * width + x;
                float fx = (x < width/2 ? x : x - width) / (float)width;
                float fy = (y < height/2 ? y : y - height) / (float)height;
                float freq = sqrt(fx * fx + fy * fy);
                if (freq < lowCutoffFreq || freq > highCutoffFreq) {
                    out[idx][0] = 0.0;
                    out[idx][1] = 0.0;
                }
            }
        }

        fftw_execute(inverse);

        points.clear();
        float xScale = 10.0f / width;
        float yScale = 10.0f / height;
        float norm = 1.0f / (width * height);
        float zMinVal = std::numeric_limits<float>::max();
        float zMaxVal = std::numeric_limits<float>::lowest();
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                int idx = y * width + x;
                float z = in[idx][0] * norm;
                z = std::max(-FLT_MAX/2.0f, std::min(z, FLT_MAX/2.0f));  // Clamp to prevent overflow
                zMap[idx] = z;
                float xPos = (x - width/2.0f) * xScale;
                float yPos = (y - height/2.0f) * yScale;
                points.push_back({xPos, yPos, z});
                zMinVal = std::min(zMinVal, z);
                zMaxVal = std::max(zMaxVal, z);
            }
        }

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(Point3D), points.data(), GL_STATIC_DRAW);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error after applying filter: " << err << std::endl;
        }

        fftw_destroy_plan(forward);
        fftw_destroy_plan(inverse);
        fftw_free(in);
        fftw_free(out);

        zMin = zMinVal;
        zMax = zMaxVal;
        histogramNeedsUpdate = true;
    }

    void updateTiffFiles(const std::string& folderPath) {
        tiffFiles.clear();
        DIR* dir = opendir(folderPath.c_str());
        if (!dir) {
            errorMessage = "Failed to open directory: " + folderPath;
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.length() > 4 && 
                (strcmp(filename.c_str() + filename.length() - 4, ".tif") == 0 || 
                 strcmp(filename.c_str() + filename.length() - 5, ".tiff") == 0)) {
                tiffFiles.push_back(filename);
            }
        }
        closedir(dir);
        errorMessage.clear();
    }

public:
    ScanViewer() {
        if (!glfwInit()) {
            std::cerr << "Failed to initialize GLFW" << std::endl;
            exit(EXIT_FAILURE);
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);

        window = glfwCreateWindow(1280, 720, "3D Scan Viewer", NULL, NULL);
        if (!window) {
            std::cerr << "Failed to create GLFW window" << std::endl;
            glfwTerminate();
            exit(EXIT_FAILURE);
        }

        glfwMakeContextCurrent(window);
        if (glewInit() != GLEW_OK) {
            std::cerr << "Failed to initialize GLEW" << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            exit(EXIT_FAILURE);
        }

        glfwSetWindowUserPointer(window, this);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        glfwSetCursorPosCallback(window, cursorPosCallback);
        glfwSetScrollCallback(window, scrollCallback);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        (void)io;

        if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 130")) {
            std::cerr << "Failed to initialize ImGui" << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            exit(EXIT_FAILURE);
        }

        shaderProgram = createShaderProgram();
        if (shaderProgram == 0) {
            std::cerr << "Failed to create shader program" << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            exit(EXIT_FAILURE);
        }

        loadDefaultData();

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(Point3D), points.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point3D), (void*)0);
        glEnableVertexAttribArray(0);
        GLenum err = glGetError();
        if (err != GL_NO_ERROR) {
            std::cerr << "OpenGL error during initialization: " << err << std::endl;
        }

        filterParams.push_back(0.5f);
        glPointSize(2.0f);
    }

    ~ScanViewer() {
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(shaderProgram);
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }

    GLuint createShaderProgram() {
        GLuint vertexShader = glCreateShader(GL_VERTEX_SHADER);
        glShaderSource(vertexShader, 1, &vertexShaderSource, NULL);
        glCompileShader(vertexShader);
        GLint success;
        glGetShaderiv(vertexShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(vertexShader, 512, NULL, infoLog);
            std::cerr << "Vertex shader compilation failed: " << infoLog << std::endl;
            return 0;
        }

        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
        glCompileShader(fragmentShader);
        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
            std::cerr << "Fragment shader compilation failed: " << infoLog << std::endl;
            glDeleteShader(vertexShader);
            return 0;
        }

        GLuint program = glCreateProgram();
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetProgramInfoLog(program, 512, NULL, infoLog);
            std::cerr << "Shader program linking failed: " << infoLog << std::endl;
            glDeleteShader(vertexShader);
            glDeleteShader(fragmentShader);
            return 0;
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return program;
    }

    void run() {
        const char* controlItems[] = {"Controls", "Data", "About"};
        const char* lutItems[] = {"Jet", "Viridis", "Plasma", "Hot", "Cool", "Turbo"};
        int currentItem = 0;
        std::string selectedFolder;

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("Viewer Controls");
            ImGui::Combo("View", &currentItem, controlItems, IM_ARRAYSIZE(controlItems));

            if (currentItem == 0) {  // Controls panel
                ImGui::SliderFloat("Z Scale", &zScale, 0.001f, 100.0f, "%.3f", ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Z Min", &zMin, rawZMin, zMax);
                ImGui::SliderFloat("Z Max", &zMax, zMin, rawZMax);
                ImGui::Combo("Color LUT", &colorLUT, lutItems, IM_ARRAYSIZE(lutItems));
                if (ImGui::CollapsingHeader("Histogram")) {
                    if (!rawZMap.empty()) {
                        const int bins = 100;
                        std::vector<float> hist(bins, 0.0f);
                        float binWidth = (rawZMax - rawZMin) / bins;
                        for (float z : rawZMap) {
                            int bin = std::min(static_cast<int>((z - rawZMin) / binWidth), bins - 1);
                            if (bin >= 0 && bin < bins) hist[bin]++;
                        }
                        float maxCount = *std::max_element(hist.begin(), hist.end());
                        if (maxCount > 0) {
                            for (float& count : hist) count /= maxCount;  // Normalize to 0-1
                        }
                        ImGui::PlotHistogram("Z Histogram", hist.data(), bins, 0, nullptr, 0.0f, 1.0f, ImVec2(0, 100));
                        ImGui::SliderFloat2("Filter Range (μm)", &filterLowCutoff, rawZMin, rawZMax, "%.3f");
                        if (ImGui::IsItemEdited()) {
                            applyFourierFilter();
                        }
                    }
                }
            }
            else if (currentItem == 1) {  // Data panel
                ImGui::InputText("Folder Path", folderPathBuffer, IM_ARRAYSIZE(folderPathBuffer));
                if (ImGui::Button("Browse Folder")) {
                    std::string path(folderPathBuffer);
                    if (!path.empty()) {
                        selectedFolder = path;
                        updateTiffFiles(path);
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Load Default Data")) {
                    loadDefaultData();
                }

                if (!tiffFiles.empty()) {
                    ImGui::Text("TIFF Files in %s:", selectedFolder.c_str());
                    for (const auto& file : tiffFiles) {
                        if (ImGui::Button(file.c_str())) {
                            std::string fullPath = selectedFolder + "/" + file;
                            if (!loadTiffZMap(fullPath)) {
                                loadDefaultData();
                            }
                        }
                    }
                } else if (!selectedFolder.empty()) {
                    ImGui::Text("No TIFF files found in %s", selectedFolder.c_str());
                }

                ImGui::Text("Current data source: %s", currentDataSource.c_str());
                if (!errorMessage.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", errorMessage.c_str());
                }
            }
            else {  // About panel
                ImGui::Text("3D Scan Viewer");
                ImGui::Text("This program visualizes 3D point cloud data.");
                ImGui::Text("Features:");
                ImGui::BulletText("Browse and load TIFF files from a folder via GUI");
                ImGui::BulletText("Interactive 3D view with mouse rotation (left-click), zoom (scroll), and pan (shift + left-click)");
                ImGui::BulletText("Adjustable Z scaling and multiple color LUTs");
                ImGui::BulletText("Bandpass Fourier filter via histogram");
                ImGui::BulletText("Collapsible histogram of Z values");
                ImGui::Text("Current data source: %s", currentDataSource.c_str());
                if (!errorMessage.empty()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), "Error: %s", errorMessage.c_str());
                }
            }
            ImGui::End();

            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);

            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
            glViewport(0, 0, width, height);
            projection = glm::perspective(glm::radians(45.0f), (float)width/height, 0.1f, 100.0f);
            view = glm::translate(glm::mat4(1.0f), glm::vec3(panX, panY, -zoom));
            view = glm::rotate(view, glm::radians(rotX), glm::vec3(1, 0, 0));
            view = glm::rotate(view, glm::radians(rotY), glm::vec3(0, 1, 0));
            model = glm::mat4(1.0f);
            glm::mat4 mvp = projection * view * model;

            glUseProgram(shaderProgram);
            glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "mvp"), 1, GL_FALSE, glm::value_ptr(mvp));
            glUniform1f(glGetUniformLocation(shaderProgram, "zMin"), zMin);
            glUniform1f(glGetUniformLocation(shaderProgram, "zMax"), zMax);
            glUniform1f(glGetUniformLocation(shaderProgram, "zScale"), zScale);
            glUniform1i(glGetUniformLocation(shaderProgram, "colorLUT"), colorLUT);

            glBindVertexArray(vao);
            glDrawArrays(GL_POINTS, 0, points.size());
            GLenum err = glGetError();
            if (err != GL_NO_ERROR) {
                std::cerr << "OpenGL error after rendering: " << err << std::endl;
            }

            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
        }
    }
};

int main() {
    ScanViewer viewer;
    viewer.run();
    return 0;
}