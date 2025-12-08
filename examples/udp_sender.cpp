#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>

// Simple UDP sender for testing
// Sends test packets to multicast group
int main(int argc, char** argv) {
    const char* multicast_group = argc > 1 ? argv[1] : "239.1.1.1";
    int port = argc > 2 ? std::atoi(argv[2]) : 10000;
    int rate = argc > 3 ? std::atoi(argv[3]) : 1000;  // packets per second
    
    std::cout << "UDP Sender\n";
    std::cout << "==========\n";
    std::cout << "Target: " << multicast_group << ":" << port << "\n";
    std::cout << "Rate: " << rate << " packets/sec\n\n";
    
    // Create socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create socket\n";
        return 1;
    }
    
    // Set multicast TTL
    unsigned char ttl = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    
    // Setup destination address
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, multicast_group, &addr.sin_addr);
    
    // Send loop
    uint64_t seq = 0;
    const auto interval = std::chrono::microseconds(1000000 / rate);
    auto next_send = std::chrono::steady_clock::now();
    
    while (true) {
        // Create test packet
        char buffer[1024];
        int len = snprintf(buffer, sizeof(buffer), 
                          "TEST_PACKET seq=%lu timestamp=%ld", 
                          seq++,
                          std::chrono::system_clock::now().time_since_epoch().count());
        
        // Send packet
        ssize_t sent = sendto(sock, buffer, len, 0,
                             (struct sockaddr*)&addr, sizeof(addr));
        
        if (sent < 0) {
            std::cerr << "Send failed\n";
            break;
        }
        
        // Rate limiting
        next_send += interval;
        std::this_thread::sleep_until(next_send);
        
        // Print progress
        if (seq % 1000 == 0) {
            std::cout << "Sent " << seq << " packets\n";
        }
    }
    
    close(sock);
    return 0;
}