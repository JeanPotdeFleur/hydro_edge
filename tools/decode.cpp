#include <iostream>
#include <fstream>
#include <vector>
#include <opencv2/opencv.hpp>

// Résolution exacte du capteur Sony IMX273 (16.1 MP) de la caméra BFS-U3-161S7C
const int WIDTH = 5320;
const int HEIGHT = 3032;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: ./decode <fichier.raw>" << std::endl;
        return -1;
    }

    std::string filename = argv[1];
    
    // 1. Lecture du binaire brut en RAM
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "Erreur I/O: Fichier introuvable." << std::endl;
        return -1;
    }

    std::vector<uint8_t> buffer(WIDTH * HEIGHT);
    file.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
    file.close();

    // 2. Encapsulation OpenCV (Zero-copy vers cv::Mat)
    cv::Mat raw_image(HEIGHT, WIDTH, CV_8UC1, buffer.data());

    // 3. Dématriçage Bayer vers BGR (Spécifique aux capteurs couleur FLIR)
    cv::Mat bgr_image;
    cv::cvtColor(raw_image, bgr_image, cv::COLOR_BayerBG2BGR);

    // 4. Export PNG
    std::string out_filename = filename + ".png";
    cv::imwrite(out_filename, bgr_image);
    
    std::cout << "[OPENCV] Conversion réussie : " << out_filename << std::endl;
    return 0;
}