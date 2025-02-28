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
        if (button == GLFW_MOUSE_BUTTON_LEFT) {
            viewer->mouseDragging = (action == GLFW_PRESS);
        }
    }

    static void cursorPosCallback(GLFWwindow* window, double xpos, double ypos) {
        ScanViewer* viewer = static_cast<ScanViewer*>(glfwGetWindowUserPointer(window));
        if (viewer->mouseDragging) {
            double dx = xpos - viewer->lastX;
            double dy = ypos - viewer->lastY;
            viewer->rotY += dx * 0.2f;
            viewer->rotX += dy * 0.2f;
        }
        viewer->lastX = xpos;
        viewer->lastY = ypos;
    }

    static void scrollCallback(GLFWwindow* window, double xoffset, double yoffset) {
        ScanViewer* viewer = static_cast<ScanViewer*>(glfwGetWindowUserPointer(window));
        viewer->zoom -= yoffset * 0.5f;
        viewer->zoom = glm::clamp(viewer->zoom, 1.0f, 20.0f);
    }

public:
    ScanViewer() {
        glfwInit();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        window = glfwCreateWindow(1280, 720, "3D Scan Viewer", NULL, NULL);
        glfwMakeContextCurrent(window);
        glewInit();

        glfwSetWindowUserPointer(window, this);
        glfwSetMouseButtonCallback(window, mouseButtonCallback);
        glfwSetCursorPosCallback(window, cursorPosCallback);
        glfwSetScrollCallback(window, scrollCallback);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 330");

        shaderProgram = createShaderProgram();

        // Generate more interesting sample data
        for (int i = -50; i <= 50; i++) {
            for (int j = -50; j <= 50; j++) {
                float x = i * 0.1f;
                float y = j * 0.1f;
                float z = sin(x) * cos(y) + sin(sqrt(x*x + y*y)) * 0.5f;
                points.push_back({x, y, z});
            }
        }

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

        GLuint fragmentShader = glCreateShader(GL_FRAGMENT_SHADER);
        glShaderSource(fragmentShader, 1, &fragmentShaderSource, NULL);
        glCompileShader(fragmentShader);

        GLuint program = glCreateProgram();
        glAttachShader(program, vertexShader);
        glAttachShader(program, fragmentShader);
        glLinkProgram(program);

        glDeleteShader(vertexShader);
        glDeleteShader(fragmentShader);
        return program;
    }

    void run() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            ImGui::Begin("Controls");
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
            ImGui::End();

            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glEnable(GL_DEPTH_TEST);

            int width, height;
            glfwGetFramebufferSize(window, &width, &height);
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