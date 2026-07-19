#include <iostream>
#include <thread>
#include <pthread.h>
#include <memory>
#include <fstream>
#include <string>
#include <csignal>
#include <atomic>

#include "Spinnaker.h"
#include "SpinGenApi/SpinnakerGenApi.h"
#include "RingBuffer.h"

using namespace Spinnaker;
using namespace Spinnaker::GenApi;
using namespace Spinnaker::GenICam;

std::atomic<bool> keep_running{true};

void sigint_handler(int signum) {
    std::cout << "\n[SYSTEM] Signal SIGINT (" << signum << ") intercepté. Amorçage du Graceful Shutdown..." << std::endl;
    keep_running = false;
}

// --- THREAD 2 : CONSUMER (Écriture POSIX) ---
void consumer_thread(std::shared_ptr<RingBuffer> buffer) {
    pthread_t thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(3, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "[CRITIQUE] Échec d'allocation d'affinité sur le Cœur 3." << std::endl;
    }

    StereoFrame current_frame;
    std::cout << "[THREAD 2] Consumer actif sur le Cœur 3. Prêt pour les I/O POSIX..." << std::endl;
    
    int frame_count = 0;

    while (buffer->pop(current_frame)) {
        std::string file0 = "data/cam0_" + std::to_string(current_frame.timestamp) + ".raw";
        std::string file1 = "data/cam1_" + std::to_string(current_frame.timestamp) + ".raw";

        std::ofstream out0(file0, std::ios::binary);
        if (out0) {
            out0.write(reinterpret_cast<const char*>(current_frame.cam0_data.data()), current_frame.cam0_data.size());
            out0.close();
        }

        std::ofstream out1(file1, std::ios::binary);
        if (out1) {
            out1.write(reinterpret_cast<const char*>(current_frame.cam1_data.data()), current_frame.cam1_data.size());
            out1.close();
        }

        frame_count++;
        std::cout << "[I/O] Frame " << frame_count << " déchargée sur le stockage. TS: " << current_frame.timestamp << std::endl;
    }
    
    std::cout << "[THREAD 2] RAM purgée intégralement. Consumer arrêté proprement." << std::endl;
}

// --- THREAD 1 : PRODUCER (Acquisition Matérielle) ---
void producer_thread(SystemPtr system, std::shared_ptr<RingBuffer> buffer) {
    pthread_t thread = pthread_self();
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(2, &cpuset);
    if (pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset) != 0) {
        std::cerr << "[CRITIQUE] Échec d'allocation d'affinité sur le Cœur 2." << std::endl;
    }

    std::cout << "[THREAD 1] Producer actif sur le Cœur 2. Début de l'ingestion..." << std::endl;

    // --- BLOC D'ISOLATION MATÉRIELLE (Garantit la destruction des pointeurs) ---
    {
        CameraList camList = system->GetCameras();
        
        if (camList.GetSize() < 2) {
            std::cerr << "[CRITIQUE] Topologie incomplète." << std::endl;
            camList.Clear();
            buffer->shutdown();
            return;
        }

        CameraPtr cam0 = camList.GetByIndex(0);
        CameraPtr cam1 = camList.GetByIndex(1);

        try {
            cam0->Init();
            cam1->Init();

            cam0->AcquisitionFrameRateEnable.SetValue(true);
            cam0->AcquisitionFrameRate.SetValue(2.0);
            cam1->AcquisitionFrameRateEnable.SetValue(true);
            cam1->AcquisitionFrameRate.SetValue(2.0);

            cam0->BeginAcquisition();
            cam1->BeginAcquisition();

            std::cout << "[THREAD 1] Boucle d'ingestion active. (Appuyez sur Ctrl+C pour arrêter)" << std::endl;

            while (keep_running) {
                try {
                    ImagePtr img0 = cam0->GetNextImage(1000);
                    ImagePtr img1 = cam1->GetNextImage(1000);

                    if (!img0->IsIncomplete() && !img1->IsIncomplete()) {
                        const uint8_t* pData0 = static_cast<const uint8_t*>(img0->GetData());
                        const uint8_t* pData1 = static_cast<const uint8_t*>(img1->GetData());
                        uint64_t timestamp = img0->GetTimeStamp();

                        buffer->push(pData0, pData1, timestamp);
                    }
                    
                    // Destruction explicite des trames
                    img0 = nullptr;
                    img1 = nullptr;
                }
                catch (Spinnaker::Exception& e) {
                    if (keep_running) std::cerr << "[EXCEPTION RUNTIME] " << e.what() << std::endl;
                }
            }

            std::cout << "[THREAD 1] Démantèlement de l'API Spinnaker..." << std::endl;
            cam0->EndAcquisition();
            cam1->EndAcquisition();
            cam0->DeInit();
            cam1->DeInit();
        }
        catch (Spinnaker::Exception& e) {
            std::cerr << "[EXCEPTION PIPELINE MATÉRIEL] " << e.what() << std::endl;
        }

        // Nettoyage forcé à la fin du bloc
        cam0 = nullptr;
        cam1 = nullptr;
        camList.Clear();
    } 
    // --- FIN DU BLOC : Tout objet Spinnaker local au Thread 1 est mathématiquement détruit.

    // Notification de fin d'ingestion au Thread 2
    buffer->shutdown();
}

int main() {
    std::signal(SIGINT, sigint_handler);
    SystemPtr system = System::GetInstance();
    size_t payload_bytes = 0;

    // --- BLOC D'ISOLATION PROBE ---
    {
        CameraList camList = system->GetCameras();
        if (camList.GetSize() == 0) {
            std::cerr << "[ERREUR FATALE] Aucune caméra détectée." << std::endl;
            camList.Clear();
            system->ReleaseInstance();
            return -1;
        }

        CameraPtr pCam = camList.GetByIndex(0);
        pCam->Init();
        payload_bytes = pCam->PayloadSize.GetValue();
        pCam->DeInit();
        
        pCam = nullptr;
        camList.Clear();
    }
    // --- FIN DU BLOC PROBE

    size_t RING_BUFFER_SIZE = 60;
    auto ring_buffer = std::make_shared<RingBuffer>(RING_BUFFER_SIZE, payload_bytes);

    std::thread t2(consumer_thread, ring_buffer);
    std::thread t1(producer_thread, system, ring_buffer);

    t1.join();
    t2.join();

    system->ReleaseInstance();
    std::cout << "[SYSTEM] Processus terminé avec succès." << std::endl;
    return 0;
}