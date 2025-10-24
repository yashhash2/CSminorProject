#include <iostream>
#include <thread>
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/mman.h>
#include <semaphore.h>
#include <algorithm>
#include <map>
#include <mutex>

const char* SHM_NAME = "/chat_shm";
const char* SEM_NAME = "/chat_sem";
const int MAX_USERS = 10;
const int MSG_SIZE = 256;
const char* PIPE_C2S_PREFIX = "/tmp/chat_c2s_";
const char* PIPE_S2C_PREFIX = "/tmp/chat_s2c_";

struct SharedData {
    int active_users[MAX_USERS];
    char users[MAX_USERS][MSG_SIZE];
};

std::map<int, int> s2c_fds;  // user_id -> fd for server->client pipe
std::mutex fds_mutex;

std::string c2s_name(int user_id) {
    return std::string(PIPE_C2S_PREFIX) + std::to_string(user_id);
}
std::string s2c_name(int user_id) {
    return std::string(PIPE_S2C_PREFIX) + std::to_string(user_id);
}

void broadcast_message(const std::string& message, int sender_id, SharedData* shm_data, sem_t* sem) {
    std::string send_data = message;
    if (send_data.back() != '\n') send_data += "\n";

    std::lock_guard<std::mutex> lock(fds_mutex);
    
    for (int i = 0; i < MAX_USERS; ++i) {
        sem_wait(sem);
        bool is_active = (shm_data->active_users[i] == 1);
        sem_post(sem);
        
        if (!is_active || i == sender_id) continue;
        
        auto it = s2c_fds.find(i);
        if (it == s2c_fds.end()) {
            std::cerr << "No pipe fd for user " << i << std::endl;
            continue;
        }
        
        int fd = it->second;
        ssize_t w = write(fd, send_data.c_str(), send_data.size());
        if (w == -1) {
            std::cerr << "Failed to write to user " << i << std::endl;
        } else {
            std::cout << "Sent to user " << i << ": " << send_data;
        }
    }
}

void handle_client(int user_id, SharedData* shm_data, sem_t* sem) {
    std::string in_pipe = c2s_name(user_id);
    std::string out_pipe = s2c_name(user_id);

    // Wait for client to create the c2s pipe
    for (int i = 0; i < 100; ++i) {
        struct stat st;
        if (stat(in_pipe.c_str(), &st) == 0) break;
        usleep(50000);
    }

    // Open c2s for reading
    int in_fd = open(in_pipe.c_str(), O_RDONLY);
    if (in_fd == -1) {
        std::cerr << "Failed to open c2s pipe for user " << user_id << "\n";
        sem_wait(sem);
        shm_data->active_users[user_id] = 0;
        sem_post(sem);
        return;
    }

    // Wait for client to create the s2c pipe
    for (int i = 0; i < 100; ++i) {
        struct stat st;
        if (stat(out_pipe.c_str(), &st) == 0) break;
        usleep(50000);
    }

    // Open s2c for writing and keep it open
    int out_fd = open(out_pipe.c_str(), O_WRONLY);
    if (out_fd == -1) {
        std::cerr << "Failed to open s2c pipe for user " << user_id << "\n";
        close(in_fd);
        sem_wait(sem);
        shm_data->active_users[user_id] = 0;
        sem_post(sem);
        return;
    }

    // Store the fd
    {
        std::lock_guard<std::mutex> lock(fds_mutex);
        s2c_fds[user_id] = out_fd;
    }

    std::cout << "Connected to user " << user_id << std::endl;

    std::string buf;
    char ch;
    while (true) {
        ssize_t r = read(in_fd, &ch, 1);
        if (r <= 0) {
            std::cerr << "User " << user_id << " disconnected\n";

            // Remove fd and mark inactive
            {
                std::lock_guard<std::mutex> lock(fds_mutex);
                s2c_fds.erase(user_id);
            }

            sem_wait(sem);
            shm_data->active_users[user_id] = 0;
            shm_data->users[user_id][0] = '\0';
            sem_post(sem);

            close(in_fd);
            close(out_fd);
            break;
        }

        if (ch == '\n') {
            sem_wait(sem);
            std::string sender = shm_data->users[user_id];
            sem_post(sem);

            std::string message = sender + ": " + buf;
            std::cout << "Received: " << message << std::endl;

            broadcast_message(message, user_id, shm_data, sem);
            buf.clear();
        } else {
            buf.push_back(ch);
            if (buf.size() >= MSG_SIZE - 1) buf.resize(MSG_SIZE - 2);
        }
    }
}

int main() {
    // Clean up old resources
    shm_unlink(SHM_NAME);
    sem_unlink(SEM_NAME);
    
    int shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        return 1;
    }
    ftruncate(shm_fd, sizeof(SharedData));
    SharedData* shm_data = (SharedData*)mmap(nullptr, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_data == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    sem_t* semaphore = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (semaphore == SEM_FAILED) {
        perror("sem_open");
        return 1;
    }

    // Initialize
    sem_wait(semaphore);
    for (int i = 0; i < MAX_USERS; ++i) {
        shm_data->active_users[i] = 0;
        shm_data->users[i][0] = '\0';
    }
    sem_post(semaphore);

    std::vector<std::thread> threads;
    std::cout << "Chat server started. Waiting for users...\n";

    while (true) {
        int user_id = -1;
        sem_wait(semaphore);
        for (int i = 0; i < MAX_USERS; ++i) {
            if (shm_data->active_users[i] == 0 && strlen(shm_data->users[i]) > 0) {
                user_id = i;
                shm_data->active_users[i] = 1;
                std::cout << "User registered: " << shm_data->users[i] << " (id=" << user_id << ")\n";
                break;
            }
        }
        sem_post(semaphore);

        if (user_id != -1) {
            threads.emplace_back(handle_client, user_id, shm_data, semaphore);
        }
        
        usleep(100000);
    }

    for (auto& t : threads) if (t.joinable()) t.join();

    munmap(shm_data, sizeof(SharedData));
    shm_unlink(SHM_NAME);
    sem_close(semaphore);
    sem_unlink(SEM_NAME);
    return 0;
}