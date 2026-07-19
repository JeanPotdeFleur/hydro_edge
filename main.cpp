#include <iostream>
#include <opencv2/opencv.hpp>
#include "Spinnaker.h"

int main() {
    std::cout << "Architecture C++ (ARMv8) opérationnelle." << std::endl;
    std::cout << "Version OpenCV : " << CV_VERSION << std::endl;
    
    // Test d'allocation et de liaison de l'API matérielle FLIR
    Spinnaker::SystemPtr system = Spinnaker::System::GetInstance();
    std::cout << "SDK Spinnaker lié avec succès." << std::endl;
    system->ReleaseInstance();
    
    return 0;
}