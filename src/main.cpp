#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Uso: " << argv[0] << " <archivo.torrent>\n";
        return 1;
    }

    std::string_view torrent_path = argv[1];
    std::cout << "BitTorrent Client inicializado.\n";
    std::cout << "Archivo objetivo: " << torrent_path << '\n';

    return 0;
}