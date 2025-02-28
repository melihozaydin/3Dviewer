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

struct Point3D {
    float x, y, z;
};

class ScanViewer {
private:
    GLFWwindow* window;
    std::vector<Point3D> points;
    glm::mat4 projection, view, model;
    float zoom = 5.0f;
    float rotX = 0.0f, rotY = 0.0f;
    float panX = 0.0f, panY = 0.0f;
    float zMin = -1.0f, zMax = 1.0f;
    float zScale = 1.0f;
    std::vector<float> filterParams;
    GLuint shaderProgram, vao, vbo;
    bool mouseDragging = false;
    double lastX = 0.0, lastY = 0.0;
    std::string currentDataSource = "Generated sample data";
    char folderPathBuffer[256] = "";
    std::vector<std::string> tiffFiles;
    std::string errorMessage;

    const char* vertexShaderSource = R"(
        #version 330 core
        layout(location = 0) in vec3 aPos;
        uniform mat4 mvp;
        uniform float zMin;
        uniform float zMax;
        uniform float zScale;
        out vec3 vColor;
        void main() {
            vec3 scaledPos = vec3(aPos.x, aPos.y, aPos.z * zScale);
            gl_Position = mvp * vec4(scaledPos, 1.0);
            float t = clamp((scaledPos.z - zMin) / (zMax - zMin), 0.0, 1.0);
            vColor = mix(vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0), t);
        }
    )";

    const char* fragmentShaderSource = R"(
        #version 330 core
        in vec3 vColor;
        out vec4 FragColor;
        void main() {
            FragColor = vec4(vColor, 1.0);
        }
    )";

    static void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
        ScanViewer* viewer = static_cast<ScanViewer*>(glfwGetWindowUserPointer(window));
        if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_PRESS && !ImGui::GetIO().WantCaptureMouse) {
            viewer->mouseDragging = true;
            glfwGetCursorPos(window, &viewer->lastX, &viewer->lastY);
        } else if (button == GLFW_MOUSE_BUTTON_LEFT && action == GLFW_RELEASE) {
            viewer->mouseDragging = false;
        }
    }

    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
        ScanViewer* viewer = static_cast<ScanViewer*>(glfwGetWindowUserPointer(window));
        if (viewer->mouseDragging && !ImGui::GetIO().WantCaptureMouse) {
            double dx = xpos - viewer->lastX;
            double dy = ypos - viewer->lastY;
            viewer->rotY += static_cast<float>(dx * 0.2);
            viewer->rotX += static_cast<float>(dy * 0.2);
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

        uint32_t width, height;
        if (!TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width) || !TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height)) {
            errorMessage = "Failed to read TIFF dimensions: " + path;
            TIFFClose(tif);
            return false;
        }

        std::vector<uint16_t> buffer(width * height);
        if (TIFFReadRGBAImage(tif, width, height, reinterpret_cast<uint32_t*>(buffer.data()), 0) == 0) {
            errorMessage = "Failed to read TIFF data: " + path;
            TIFFClose(tif);
            return false;
        }

        points.clear();
        float xScale = 10.0f / width;
        float yScale = 10.0f / height;
        float zScaleFactor = 2.0f / 65535.0f;

        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t idx = y * width + x;
                uint16_t gray = buffer[idx] & 0xFFFF;
                float z = (gray * zScaleFactor) - 1.0f;
                float xPos = (x - width/2.0f) * xScale;
                float yPos = (y - height/2.0f) * yScale;
                points.push_back({xPos, yPos, z});
            }
        }

        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(Point3D), points.data(), GL_STATIC_DRAW);

        TIFFClose(tif);
        currentDataSource = path;
        zMin = -1.0f;
        zMax = 1.0f;
        errorMessage.clear();
        std::cout << "Loaded TIFF: " << width << "x" << height << " (" << points.size() << " points)" << std::endl;
        return true;
    }

    void loadDefaultData() {
        points.clear();
        for (int i = -50; i <= 50; i++) {
            for (int j = -50; j <= 50; j++) {
                float x = i * 0.1f;
                float y = j * 0.1f;
                float z = sin(x) * cos(y) + sin(sqrt(x*x + y*y)) * 0.5f;
                points.push_back({x, y, z});
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(Point3D), points.data(), GL_STATIC_DRAW);
        currentDataSource = "Generated sample data";
        errorMessage.clear();
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
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

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

        if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 330")) {
            std::cerr << "Failed to initialize ImGui" << std::endl;
            glfwDestroyWindow(window);
            glfwTerminate();
            exit(EXIT_FAILURE);
        }

        shaderProgram = createShaderProgram();
        loadDefaultData();

        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(Point3D), points.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Point3D), (void*)0);
        glEnableVertexAttribArray(0);

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
        }

        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
        glCompileShader(fragmentShader);
        glGetShaderiv(fragmentShader, GL_COMPILE_STATUS, &success);
        if (!success) {
            char infoLog[512];
            glGetShaderInfoLog(fragmentShader, 512, NULL, infoLog);
            std::cerr << "Fragment shader compilation failed: " << infoLog << std::endl;
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
        }

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return program;
    }

    void run() {
        const char* controlItems[] = {"Controls", "Data", "About"};
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
                ImGui::SliderFloat("Z Min", &zMin, -2.0f, zMax);
                ImGui::SliderFloat("Z Max", &zMax, zMin, 2.0f);
                ImGui::SliderFloat("Z Scale", &zScale, 0.1f, 5.0f);
                ImGui::SliderFloat("Zoom", &zoom, 1.0f, 20.0f);
                ImGui::SliderFloat("Rotate X", &rotX, -180.0f, 180.0f);
                ImGui::SliderFloat("Rotate Y", &rotY, -180.0f, 180.0f);
                ImGui::SliderFloat("Pan X", &panX, -10.0f, 10.0f);
                ImGui::SliderFloat("Pan Y", &panY, -10.0f, 10.0f);

                if (ImGui::Button("Add Filter")) {
                    filterParams.push_back(0.5f);
                }
                for (size_t i = 0; i < filterParams.size(); i++) {
                    std::string label = "Filter " + std::to_string(i);
                    ImGui::SliderFloat(label.c_str(), &filterParams[i], 0.0f, 1.0f);
                }

                ImGui::Text("Camera Controls:");
                ImGui::Text("- Left Click + Drag: Rotate");
                ImGui::Text("- Scroll Wheel: Zoom");
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
                                loadDefaultData();  // Revert to default on failure
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
                ImGui::BulletText("Interactive 3D view with rotation and zoom");
                ImGui::BulletText("Adjustable Z scaling and color mapping");
                ImGui::BulletText("Custom filters for data processing");
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

            glBindVertexArray(vao);
            glDrawArrays(GL_POINTS, 0, points.size());

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