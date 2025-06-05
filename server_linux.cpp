// server_linux.cpp
#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <ctime>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fstream>

struct ClientInfo {
    std::string domain;
    std::string machine;
    std::string ip;
    std::string user;
    time_t lastActive;
    int socket;
};

std::vector<ClientInfo> clients;
std::mutex clientsMutex;

void SaveScreenshot(const std::string& machine, const std::vector<char>& jpegData) {
    std::string filename = "screenshot_" + machine + "_" + std::to_string(time(nullptr)) + ".jpg";
    std::ofstream file(filename, std::ios::binary);
    file.write(jpegData.data(), jpegData.size());
    file.close();
    std::cout << "Скриншот сохранён: " << filename << std::endl;
    
    // Можно сразу открыть для просмотра (если есть xdg-open)
    system(("xdg-open " + filename + " &").c_str());
}

void HandleClient(int clientSocket, sockaddr_in clientAddr) {
    char buffer[4096];
    int bytesReceived;
    
    // Получаем информацию о клиенте
    bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
    if (bytesReceived <= 0) {
        close(clientSocket);
        return;
    }
    
    buffer[bytesReceived] = '\0';
    std::string clientData(buffer);
    
    // Парсим информацию (формат: domain|machine|user)
    size_t pos1 = clientData.find('|');
    size_t pos2 = clientData.find('|', pos1 + 1);
    
    if (pos1 == std::string::npos || pos2 == std::string::npos) {
        close(clientSocket);
        return;
    }
    
    ClientInfo info;
    info.domain = clientData.substr(0, pos1);
    info.machine = clientData.substr(pos1 + 1, pos2 - pos1 - 1);
    info.user = clientData.substr(pos2 + 1);
    info.ip = inet_ntoa(clientAddr.sin_addr);
    info.lastActive = time(nullptr);
    info.socket = clientSocket;
    
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        clients.push_back(info);
    }
    
    std::cout << "Подключился клиент: " << info.domain << "/" << info.machine 
              << " (" << info.ip << ") пользователь: " << info.user << std::endl;
    
    while (true) {
        bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesReceived <= 0) {
            break;
        }
        
        // Обновляем время последней активности
        {
            std::lock_guard<std::mutex> lock(clientsMutex);
            for (auto& client : clients) {
                if (client.socket == clientSocket) {
                    client.lastActive = time(nullptr);
                    break;
                }
            }
        }
        
        // Проверяем запрос на скриншот
        if (bytesReceived >= 4 && strncmp(buffer, "SCRN", 4) == 0) {
            // Получаем данные скриншота
            std::vector<char> imageData;
            int imageSize = *(int*)(buffer + 4);
            imageData.resize(imageSize);
            
            int totalReceived = 0;
            while (totalReceived < imageSize) {
                bytesReceived = recv(clientSocket, imageData.data() + totalReceived, imageSize - totalReceived, 0);
                if (bytesReceived <= 0) break;
                totalReceived += bytesReceived;
            }
            
            if (totalReceived == imageSize) {
                SaveScreenshot(info.machine, imageData);
            }
        }
    }
    
    // Удаляем клиента
    {
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto it = clients.begin(); it != clients.end(); ++it) {
            if (it->socket == clientSocket) {
                std::cout << "Клиент отключился: " << it->machine << std::endl;
                clients.erase(it);
                break;
            }
        }
    }
    
    close(clientSocket);
}

void AdminConsole() {
    while (true) {
        std::cout << "\nМеню:\n";
        std::cout << "1. Список клиентов\n";
        std::cout << "2. Запросить скриншот\n";
        std::cout << "3. Выход\n";
        std::cout << "Выбор: ";
        
        int choice;
        std::cin >> choice;
        
        if (choice == 1) {
            std::lock_guard<std::mutex> lock(clientsMutex);
            std::cout << "\nПодключенные клиенты (" << clients.size() << "):\n";
            for (const auto& client : clients) {
                std::cout << "Домен: " << client.domain << "\n";
                std::cout << "Компьютер: " << client.machine << "\n";
                std::cout << "IP: " << client.ip << "\n";
                std::cout << "Пользователь: " << client.user << "\n";
                std::cout << "Последняя активность: " << ctime(&client.lastActive);
                std::cout << "-------------------\n";
            }
        }
        else if (choice == 2) {
            std::lock_guard<std::mutex> lock(clientsMutex);
            if (clients.empty()) {
                std::cout << "Нет подключенных клиентов.\n";
                continue;
            }
            
            std::cout << "Выберите клиента (1-" << clients.size() << "): ";
            for (size_t i = 0; i < clients.size(); ++i) {
                std::cout << "\n" << (i + 1) << ". " << clients[i].machine;
            }
            std::cout << "\nВыбор: ";
            
            int clientChoice;
            std::cin >> clientChoice;
            
            if (clientChoice < 1 || clientChoice > clients.size()) {
                std::cout << "Неверный выбор.\n";
                continue;
            }
            
            int targetSocket = clients[clientChoice - 1].socket;
            const char* request = "SCRN";
            send(targetSocket, request, 4, 0);
            
            std::cout << "Запрос скриншота отправлен.\n";
        }
        else if (choice == 3) {
            exit(0);
        }
    }
}

int main() {
    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "Ошибка создания сокета\n";
        return 1;
    }
    
    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(12345);
    
    if (bind(serverSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Ошибка привязки сокета\n";
        close(serverSocket);
        return 1;
    }
    
    if (listen(serverSocket, 5) < 0) {
        std::cerr << "Ошибка прослушивания\n";
        close(serverSocket);
        return 1;
    }
    
    std::cout << "Сервер запущен на порту 12345\n";
    
    std::thread adminThread(AdminConsole);
    adminThread.detach();
    
    while (true) {
        sockaddr_in clientAddr;
        socklen_t clientAddrSize = sizeof(clientAddr);
        int clientSocket = accept(serverSocket, (sockaddr*)&clientAddr, &clientAddrSize);
        if (clientSocket < 0) {
            std::cerr << "Ошибка принятия соединения\n";
            continue;
        }
        
        std::thread clientThread(HandleClient, clientSocket, clientAddr);
        clientThread.detach();
    }
    
    close(serverSocket);
    return 0;
}
