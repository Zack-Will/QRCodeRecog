#include <iostream>
#include <opencv2/opencv.hpp>
#include <opencv2/wechat_qrcode.hpp>
#include <thread>
#include <mutex>
#include <qrencode.h>
#include "MD5.h"
#include "package.pb.h"
#define INTERNAL_CAM_INDEX 0
#define QR_SENDBACK_DELAY_US 100
#define QR_RECV_DELAY_MS 100

using namespace cv;
using namespace std;

mutex sendBackMutex;
bool sendBackFlag;
int requestPkgID;
QRCodePackage::Package_PackageType sendBackType;
string fileMD5;
string fileName;

string getFileMD5(const string &name) {
    ifstream in(name.c_str(), ios::binary);
    if (!in)
        return "";
    MD5 md5;
    std::streamsize length;
    char buffer[512];
    while (!in.eof()) {
        in.read(buffer, 512);
        length = in.gcount();
        if (length > 0)
            md5.update(buffer, length);
    }
    in.close();
    return md5.toString();
}

void resendRequest(){
    int sendBackPkgID = 0;
    while (true){
        if (sendBackFlag){
            sendBackPkgID++;
            QRCodePackage::Package pkg;
            sendBackMutex.lock();
            sendBackFlag = false;
            pkg.set_id(sendBackPkgID);
            pkg.set_type(sendBackType);
            sendBackMutex.unlock();
            printf("Request for package: %d\n", requestPkgID);
            switch (sendBackType) {
                case QRCodePackage::Package_PackageType_BACK:
                    pkg.set_data(to_string(requestPkgID));
                    break;
                case QRCodePackage::Package_PackageType_FINI:
                    break;
                default:
                    cout << "Receiving wrong packages!" << endl;
                    break;
            }
            //将结构体序列化为字符串
            string sendStr;
            pkg.SerializeToString(&sendStr);
            // 使用qrencode进行字符串编码
            QRcode *code = QRcode_encodeString(sendStr.c_str(), 0, QR_ECLEVEL_H, QR_MODE_8, 1);
            if (code == nullptr) {
                std::cout << "code = NULL" << std::endl;
                return;
            }
            cv::Mat img = cv::Mat(code->width, code->width, CV_8U);
            for (int i = 0; i < code->width; ++i) {
                for (int j = 0; j < code->width; ++j) {
                    img.at<uchar>(i, j) = (code->data[i * code->width + j] & 0x01) == 0x01 ? 0 : 255;
                }
            }
            cv::resize(img, img, cv::Size(img.rows * 10, img.cols * 10), 0, 0, cv::INTER_NEAREST);
            cv::Mat result = cv::Mat::zeros(img.rows + 30, img.cols + 30, CV_8U);
            //白底
            result = 255 - result;
            //转换成彩色
            cv::cvtColor(result, result, cv::COLOR_GRAY2BGR);
            cv::cvtColor(img, img, cv::COLOR_GRAY2BGR);
            //建立roi
            cv::Rect roi_rect = cv::Rect((result.rows - img.rows) / 2, (result.cols - img.rows) / 2, img.cols, img.rows);
            //roi关联到目标图像，并把源图像复制到指定roi
            img.copyTo(result(roi_rect));
            cv::imshow("sendBack", result);
            cv::waitKey(QR_SENDBACK_DELAY_US);
        }
    }
}

void recvPackage(){
    ofstream ofs;
    //加载模型文件
    Ptr<wechat_qrcode::WeChatQRCode> detector;
    string detect_prototxt = "D:\\WeChatQRModel\\detect.prototxt";
    string detect_caffe_model = "D:\\WeChatQRModel\\detect.caffemodel";
    string sr_prototxt = "D:\\WeChatQRModel\\sr.prototxt";
    string sr_caffe_model = "D:\\WeChatQRModel\\sr.caffemodel";
    //生成detector
    detector = makePtr<wechat_qrcode::WeChatQRCode>(detect_prototxt, detect_caffe_model, sr_prototxt, sr_caffe_model);
    vector<Mat> vPoints;
    vector<String> strDecoded;
    vector<String> lastResult;
    bool flag = false;
    VideoCapture capture;
    capture.open(INTERNAL_CAM_INDEX, CAP_DSHOW);
    if (!capture.isOpened())
    {
        cout << "can't open camera" << endl;
        exit(-1);
    }

    Mat img;
    int lPkgID = -1;
    int cPkgID;
    int tPkgID;
    int MAX_DATA_LENGTH;
    while (capture.read(img))
    {
        double start = getTickCount();
        flag = false;
        strDecoded = detector->detectAndDecode(img, vPoints);
        if(strDecoded != lastResult && !strDecoded.empty()){
            //cout << "Result changed!" << endl;
            QRCodePackage::Package pkg;
            pkg.ParseFromString(strDecoded[0]);
            cPkgID = pkg.id();
            if(cPkgID != lPkgID + 1){
                sendBackMutex.lock();
                sendBackFlag = true;
                sendBackType = QRCodePackage::Package_PackageType_BACK;
                requestPkgID = lPkgID + 1;
                sendBackMutex.unlock();
            } else {
                lPkgID = cPkgID;
                if(sendBackFlag){
                    sendBackMutex.lock();
                    sendBackFlag = false;
                    sendBackMutex.unlock();
                }
                QRCodePackage::Package_PackageType pkgt = pkg.type();
                switch (pkgt) {
                    case QRCodePackage::Package_PackageType_HEAD:
                        tPkgID = stoi(pkg.data());
                        MAX_DATA_LENGTH = pkg.len();
                        break;
                    case QRCodePackage::Package_PackageType_HASH:
                        fileMD5 = pkg.data();
                        break;
                    case QRCodePackage::Package_PackageType_NAME:
                        fileName = pkg.data();
                        ofs.open(fileName,ios::out|ios::binary|ios::trunc);
                        break;
                    case QRCodePackage::Package_PackageType_DATA:
                        cout << "Package " << pkg.id() << " received" << endl;
                        int length;
                        if(cPkgID == lPkgID)
                            length = pkg.data().length();
                        else
                            length = MAX_DATA_LENGTH;
                        ofs.write(pkg.data().c_str(), length);
                        break;
                    default:
                        cout << "Receiving wrong packages!" << endl;
                        break;
                }
                if(cPkgID == tPkgID){
                    //关闭文件流
                    ofs.close();
                    //发送FINI
                    sendBackMutex.lock();
                    sendBackFlag = true;
                    sendBackType = QRCodePackage::Package_PackageType_FINI;
                    sendBackMutex.unlock();
                    //检查MD5
                    if(fileMD5 == getFileMD5(fileName)){
                        cout << "MD5 check passed!" << endl;
                        break;
                    } else {
                        cout << "MD5 check failed!" << endl;
                        break;
                        ofs.open(fileName,ios::out|ios::binary|ios::trunc);
                        lPkgID = -1;
                        sendBackMutex.lock();
                        sendBackFlag = true;
                        sendBackType = QRCodePackage::Package_PackageType_BACK;
                        requestPkgID = lPkgID + 1;
                        sendBackMutex.unlock();
                    }
                }
            }
        }
#if 1
        for (int i = 0; i < strDecoded.size(); i++)
        {
            Point pt1 = Point((int)vPoints[i].at<float>(0, 0), (int)vPoints[i].at<float>(0, 1));
            Point pt2 = Point((int)vPoints[i].at<float>(1, 0), (int)vPoints[i].at<float>(1, 1));
            Point pt3 = Point((int)vPoints[i].at<float>(2, 0), (int)vPoints[i].at<float>(2, 1));
            Point pt4 = Point((int)vPoints[i].at<float>(3, 0), (int)vPoints[i].at<float>(3, 1));
            line(img, pt1, pt2, Scalar(0, 255, 0), 2);
            line(img, pt2, pt3, Scalar(0, 255, 0), 2);
            line(img, pt3, pt4, Scalar(0, 255, 0), 2);
            line(img, pt4, pt1, Scalar(0, 255, 0), 2);
            putText(img, strDecoded[i], pt1, 0, 0.5, Scalar(255, 0, 0), 2);
        }
#endif
        double end = getTickCount();
        double run_time = (end - start) / getTickFrequency();
        double fps = 1 / run_time;
        putText(img, format("FPS: %0.2f", fps), Point(20, 20), FONT_HERSHEY_SIMPLEX, 0.5, Scalar(0, 0, 255), 1, 8);
        imshow("recv_window", img);
        cv::moveWindow("recv_window", 1000, 0);
        lastResult = strDecoded;
        waitKey(QR_RECV_DELAY_MS);
    }
    waitKey();
}

int main() {
    thread resendThread(resendRequest);
    resendThread.detach();
    thread recvThread(recvPackage);
    recvThread.join();
    return 0;
}