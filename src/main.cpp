#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <file.torrent>\n";
        return 1;
    }

    std::string_view torrent_path = argv[1];
    std::cout << "BitTorrent Client initialized.\n";
    std::cout << "Target file: " << torrent_path << '\n';

    return 0;
}