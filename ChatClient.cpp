#include <iostream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <semaphore.h>
#include <sys/stat.h>

const char* SHM_NAME = "/chat_shm";
const char* SEM_NAME = "/chat_sem";
const int MSG_SIZE = 256;
const int MAX_USERS = 10;
const char* PIPE_C2S_PREFIX = "/tmp/chat_c2s_";
const char* PIPE_S2C_PREFIX = "/tmp/chat_s2c_";

struct SharedData {
    int active_users[MAX_USERS];
    char users[MAX_USERS][MSG_SIZE];
};

std::string c2s_name(int user_id) {
    return std::string(PIPE_C2S_PREFIX) + std::to_string(user_id);
}
std::string s2c_name(int user_id) {
    return std::string(PIPE_S2C_PREFIX) + std::to_string(user_id);
}

void read_messages(int user_id) {
    std::string pipe_name = s2c_name(user_id);

    // Open for reading (will block until server opens for writing)
    int fd = open(pipe_name.c_str(), O_RDONLY);
    if (fd == -1) {
        std::cerr << "Cannot open s2c pipe for reading.\n";
        return;
    }

    std::cout << "Connected! Ready to receive messages.\n";

    std::string line;
    char ch;
    while (true) {
        ssize_t r = read(fd, &ch, 1);
        if (r <= 0) break;
        if (ch == '\n') {
            std::cout << "\n" << line << "\n> ";
            std::cout.flush();
            line.clear();
        } else {
            line.push_back(ch);
            if (line.size() >= MSG_SIZE - 1) line.resize(MSG_SIZE - 2);
        }
    }
    close(fd);
}

int main() {
    std::cout << "Enter your username: ";
    std::string username;
    std::getline(std::cin, username);
    if (username.empty()) username = "anon";

    int shm_fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        std::cerr << "Failed to open shared memory. Is the server running?\n";
        return 1;
    }
    SharedData* shm_data = (SharedData*)mmap(nullptr, sizeof(SharedData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (shm_data == MAP_FAILED) {
        std::cerr << "mmap failed\n";
        return 1;
    }

    sem_t* sem = sem_open(SEM_NAME, 0);
    if (sem == SEM_FAILED) {
        std::cerr << "sem_open failed\n";
        return 1;
    }

    // Find empty slot and register
    int user_id = -1;
    sem_wait(sem);
    for (int i = 0; i < MAX_USERS; ++i) {
        if (shm_data->active_users[i] == 0) {
            user_id = i;
            strncpy(shm_data->users[user_id], username.c_str(), MSG_SIZE - 1);
            shm_data->users[user_id][MSG_SIZE - 1] = '\0';
            break;
        }
    }
    sem_post(sem);

    if (user_id == -1) {
        std::cerr << "Max users reached.\n";
        return 1;
    }

    std::cout << "Registered as user " << user_id << std::endl;

    // Create s2c pipe FIRST (for receiving)
    std::string s2c_pipe = s2c_name(user_id);
    unlink(s2c_pipe.c_str());
    if (mkfifo(s2c_pipe.c_str(), 0666) == -1) {
        std::cerr << "Failed to create s2c pipe\n";
        return 1;
    }

    // Create c2s pipe (for sending)
    std::string c2s_pipe = c2s_name(user_id);
    unlink(c2s_pipe.c_str());
    if (mkfifo(c2s_pipe.c_str(), 0666) == -1) {
        std::cerr << "Failed to create c2s pipe\n";
        return 1;
    }

    // Start reader thread BEFORE opening write pipe
    std::thread reader(read_messages, user_id);
    
    // Give reader thread time to start
    usleep(100000);

    // Now open c2s for writing
    std::cout << "Connecting to server...\n";
    int write_fd = open(c2s_pipe.c_str(), O_WRONLY);
    if (write_fd == -1) {
        std::cerr << "Cannot open c2s pipe for writing.\n";
        unlink(s2c_pipe.c_str());
        if (reader.joinable()) reader.join();
        return 1;
    }

    std::cout << "Connected to server!\n";

    // Interactive loop
    std::string line;
    std::cout << "> ";
    while (std::getline(std::cin, line)) {
        if (line == "/exit") break;
        std::string out = line + "\n";
        ssize_t w = write(write_fd, out.c_str(), out.size());
        if (w == -1) {
            std::cerr << "Write failed\n";
            break;
        }
        std::cout << "> ";
    }

    close(write_fd);
    unlink(s2c_pipe.c_str());
    unlink(c2s_pipe.c_str());
    if (reader.joinable()) reader.join();

    munmap(shm_data, sizeof(SharedData));
    sem_close(sem);
    return 0;
}