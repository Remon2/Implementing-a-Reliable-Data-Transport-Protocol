#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/errno.h>
#include <sys/wait.h>
#include <unistd.h>
#include <sys/types.h>
#include <vector>
#include <pthread.h>
#include <sys/time.h>
#include <fstream>
typedef unsigned char uchar;
#ifndef ERESTART
#define ERESTART EINTR
#endif

int TIMEOUT = 300000;
int TIMERSTOP = -100;
using namespace std;
extern int errno;
ofstream logFile;
ofstream logFile2;
ofstream logFile3;
ifstream infoFile;

timeval time1;
timeval time2;

int timeCount = 0;
double startTime;
struct ack_packet
{
    uint16_t cksum; /* Optional bonus part */
    uint16_t len;
    uint32_t ackno;
};
/* Data-only packets */
struct packet
{
    /* Header */
    uint16_t cksum; /* Optional bonus part */
    uint16_t len;
    uint32_t seqno;
    /* Data */
    char data[500]; /* Not always 500 bytes, can be less */
};
int serverPort = 0;
int maxWindowSize;
int seed;
float p;
int curWindowSize=2;

void sig_func(int);
int prepareFile(char*);
void sendBySelectiveRepeat(char*);
void serve(); /* main server function */
void convertDataPacketToByte(packet*, char*);
void parseAckPacket(char *, int, ack_packet*);
void* receiveAckSelectiveRepeat(void *);
void* receiveAckStopAndWait(void *);
void *receiveAckGBN(void *threadid);
void sendByStopAndWait(char*);
void sendByGBN(char*);
void *selectiveTimer(void *time);
void *GBNTimer(void *time);
FILE* fp;
int ssth=50;

vector<char*> *frames = new vector<char*>;
vector<bool> *ackedFrames = new vector<bool>;
vector<int> *times = new vector<int>;
vector<int> *lens = new vector<int>;

int base = 0;
int curr = 0;
int childSocket;


int protocol = 2;//0 for stop and wait, 1 for selective repeat, 2 for GBN



pthread_t recThread;
pthread_t stopAndWaitTimerThread;
pthread_t SelectiveThread;

bool stopAndWaitAcked = false;
bool recThreadIsCreated = false;

pthread_mutex_t Selectivemutex;

pthread_mutex_t StopAndWaitmutex;

pthread_mutex_t StopAndWaitmutexTimer;

pthread_mutex_t StopAndWaitRecThreadCreationMutex;

pthread_mutex_t lock1;
pthread_mutex_t lock2;
void extractPacket(packet* packet, char buffer[], int buffLength);

/* serve: set up the service */
struct sockaddr_in my_addr; /* address of this service */
struct sockaddr_in child_addr; /* address of this service */
struct sockaddr_in client_addr; /* client's address */
int svc; /* listening socket providing service */
int rqst; /* socket accepting the request */
int dropCount = 0;
int noOfDroppedPackets=0;
bool isdropped()
{
    float dropCountFloat = dropCount;
    float drop = dropCountFloat * p;

    if (drop >= 0.9)
    {
        dropCount = 0;
        noOfDroppedPackets++;
        return true;
    }
    else
    {
        return false;
    }

}
uint16_t calculateChecksum(char const *buf) {
    int i = 0;

    uint32_t sum = 0;
    while (buf[i]!='\0') {
      sum += (((buf[i] << 8) & 0xFF00) | ((buf[i + 1]) & 0xFF));
      if ((sum & 0xFFFF0000) > 0) {
        sum = sum & 0xFFFF;
        sum += 1;
      }
      i += 2;
    }
    if (buf[i]!='\0') {
      sum += (buf[i] << 8 & 0xFF00);
      if ((sum & 0xFFFF0000) > 0) {
        sum = sum & 0xFFFF;
        sum += 1;
      }
    }
    sum = ~sum;
    sum = sum & 0xFFFF;
    return sum;

  }

void resend(int i)
{
    int sendFlag = 0;

    sendFlag = sendto(childSocket, frames->at(i), lens->at(i) + 8, 0,
                      (struct sockaddr *) &client_addr, sizeof(client_addr));


    packet p;
    extractPacket(&p, frames->at(i), lens->at(i));
    logFile << "Resending PACKET WITH SEQ NUM = " << p.seqno << endl;

}
void readInputFile()
{
    string line;
    ifstream myfile ("server.in");
    if (myfile.is_open())
    {

        if( getline (myfile,line) )
        {
            serverPort=atoi(line.c_str());
        }

        if( getline (myfile,line) )
        {
            maxWindowSize=atoi(line.c_str());
        }
        if( getline (myfile,line) )
        {
            seed=atoi(line.c_str());
        }
        if( getline (myfile,line) )
        {
            p=atof(line.c_str());
        }
    }

    myfile.close();
}


int checksum(char *a){

int sum=0;
int i=0;
    while(a[i]!='\0')
    {
     sum+=(int)a[i];
     i++;
    }
  sum = ~sum;
return sum;
}


int main(int argc, char** argv)
{

    logFile.open("log.txt");
    logFile2.open("congestion.txt");
    logFile3.open("congestion2.txt");
    readInputFile();
    serve();
    logFile<<"number of dropped packets = "<<noOfDroppedPackets<<endl;
    return 0;
}

void *receiveAckSelectiveRepeat(void *threadid) {

	while (1) {

		socklen_t alen = sizeof(client_addr);
		int reclen;
		char Buffer[8];
		if (reclen = (recvfrom(childSocket, Buffer, 8, 0,
				(struct sockaddr *) &client_addr, &alen)) < 0)
			perror("recvfrom() failed");
		ack_packet packet;
		parseAckPacket(Buffer, 8, &packet);
		int frameIndex = packet.ackno - base;
		pthread_mutex_lock(&Selectivemutex);
		cout << "ack " << packet.ackno << " received" << endl;
		logFile << "ACK " << packet.ackno << " received" << endl;
		if (frameIndex < 0 || ackedFrames->size() == 0) {
			cout << "dublicate ack is detected ... ignore" << endl;
			logFile << "dublicate ACK is detected ... ignore" << endl;

		} else if (ackedFrames->size() != 0
				&& ackedFrames->at(frameIndex) == true) {
			cout << "dublicate ack is detected ... ignore" << endl;
			logFile << "dublicate ACK is detected ... ignore" << endl;
		} else {
			if (curWindowSize < maxWindowSize) {
				curWindowSize++;
				logFile3<<curWindowSize<<endl;
				logFile << "INCREASE WINDOW SIZE FROM  " << curWindowSize - 1<< " TO " << curWindowSize << endl;
			}
			cout << "Buffers size = " << ackedFrames->size() << endl;
			ackedFrames->at(frameIndex) = true;
			times->at(frameIndex) = TIMERSTOP;
			if (ackedFrames->at(0) == true) {
				for (int i = 0; i <= curr - base; ++i) {
					if (ackedFrames->size() == 0)
						break;
					if (ackedFrames->at(0) == true) {
						frames->erase(frames->begin());
						ackedFrames->erase(ackedFrames->begin());
						times->erase(times->begin());
						lens->erase(lens->begin());
						++base;
					} else {
						break;
					}
				}
				logFile << "new base pointer = " << base << endl;
			}
		}
		pthread_mutex_unlock(&Selectivemutex);
	}
}


void *selectiveTimer(void *time) {

	while (true) {
		pthread_mutex_lock(&Selectivemutex);
		for (int i = 0; i < times->size(); ++i) {
			times->at(i)--;
			if(times->at(i)==0) {
				cout<<"TIME OUT PACKET: "<<base+i<<endl;
				logFile<<endl;
				logFile<<"++++++++++++TIME OUT DETECTED++++++++++++++"<<endl;
				logFile<<endl;
				times->at(i)= TIMEOUT;
				resend(i);
			}
		}

		pthread_mutex_unlock(&Selectivemutex);
	}

}

void sendBySelectiveRepeat(char* fileName) {

	int size = prepareFile(fileName);
	packet currPacket;
	cout << "File Size : " << size << endl;
	logFile << "File Size : " << size << endl;
	int packetCount = 0;

	for (int i = 0; i < size;) {

		dropCount++;
		dropCount = dropCount % 101;
		currPacket.len = fread(currPacket.data, 1, 500, fp);
		string st(currPacket.data, strlen(currPacket.data));
        vector<char> bytes(st.begin(), st.end());
        bytes.push_back('\0');
        char *c = &bytes[0];
        int n = checksum(c)& 255;
        currPacket.cksum = (uint16_t)n;
        logFile<<"check sum = "<<currPacket.cksum<<endl;
		currPacket.seqno = packetCount;
		i += currPacket.len;
		char *buffer = new char[512];
		unsigned int sendFlag = 1;
		convertDataPacketToByte(&currPacket, buffer);
		while (1) {
			pthread_mutex_lock(&Selectivemutex);
//            logFile<<"cur = "<<curr<<"base = "<<base<<"curwindow size = "<<curWindowSize<<endl;

			if (curr - base < curWindowSize-1) {
				if (!isdropped()) {
					sendFlag = sendto(childSocket, buffer, currPacket.len + 8,
							0, (struct sockaddr *) &client_addr,
							sizeof(client_addr));
					if (sendFlag < -1)
						break;
					else if (sendFlag == -1)
						perror("sendto");
				} else {
					if (curWindowSize > 2) {
						curWindowSize = curWindowSize / 2;

					} else if (curWindowSize < 2) {
						curWindowSize = 2;

					}
                    logFile2<<ssth<<endl;
                	logFile3<<curWindowSize<<endl;

                    logFile<<"there is a packet loss and the new Window size = "<<curWindowSize<<"and the ssth = "<<ssth<<endl;
                    logFile << endl;
                    logFile << "PACKET :" << packetCount<< " IS DROPPED " << endl;
                    logFile << endl;
					break;

				}
			} else {
				pthread_mutex_unlock(&Selectivemutex);
			}
		}

		logFile << "packet number:  " << currPacket.seqno << " is sent" << endl;

		curr = currPacket.seqno;
		frames->push_back(buffer);
		ackedFrames->push_back(false);
		times->push_back(TIMEOUT);
		lens->push_back(currPacket.len);
		pthread_mutex_unlock(&Selectivemutex);
		packetCount++;

	}
	currPacket.cksum = 1;
	currPacket.len = 150;
	currPacket.seqno = packetCount;
	cout << "******		Sending finish packet." << endl;
	logFile << "******		Sending finish packet." << endl;

	gettimeofday(&time2, NULL);
	long t2 = time2.tv_sec;
	t2 = t2 - time1.tv_sec;
	logFile << "Time Taken is " << t2 << " secs" << endl;
	logFile.close();
	char *dataFinish = new char[8];
	int sendFlag = 1;
	convertDataPacketToByte(&currPacket, dataFinish);

	while (1) {
		if (curr - base < curWindowSize - 1) {
			sendFlag = sendto(childSocket, dataFinish, sizeof(dataFinish), 0,
					(struct sockaddr *) &client_addr, sizeof(client_addr));
			break;
		}
	}
	pthread_mutex_lock(&Selectivemutex);

	curr = currPacket.seqno;
	frames->push_back(dataFinish);
	ackedFrames->push_back(false);
	lens->push_back(currPacket.len);
	times->push_back(TIMEOUT);
	pthread_mutex_lock(&Selectivemutex);

	if (sendFlag == -1)
		perror("sendto");
	cout << "******		Sending finish packet DONE." << endl;
	cout << "******		Finish Window size =" << curWindowSize << endl;
	logFile << "******		Finish Window size =" << curWindowSize << endl;
}

void serve()
{
    signal(SIGSEGV, sig_func);
    socklen_t alen;
    int sockoptval = 1;

    char hostname[128] = "localhost";
    if ((svc = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
    {
        perror("cannot create socket");
        exit(1);
    }

    memset((char*) &my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(serverPort);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY );

    if (bind(svc, (struct sockaddr *) &my_addr, sizeof(my_addr)) < 0)
    {
        perror("bind failed");
        exit(1);
    }
    while(1)
    {

        alen = sizeof(client_addr);
        cout << "Server is now Ready for requests :) " << endl;
        char echoBuffer[100];
        if ((recvfrom(svc, echoBuffer, 100, 0, (struct sockaddr *) &client_addr,
                      &alen)) < 0)
            cout<<"receive requested file from client failed"<<endl;
        cout << "requested file  : " << echoBuffer << endl;
        logFile << "A request is received for file : " << echoBuffer << endl;
        logFile << "FORKING NEW PROCESS TO HANDLE THE SENDING" << endl;
        int pid = fork();
        //child process will enter
        if (pid == 0)
        {
            logFile << "child process created " << endl;
            pthread_t ackThread;
            int rc;
            long t = 0;
            logFile << "creating new socket to handel the connection" << endl;
            if ((childSocket = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
            {
                cout<<"cannot create socket"<<endl;
                exit(1);
            }

            memset((char*) &child_addr, 0, sizeof(child_addr));
            child_addr.sin_family = AF_INET;
            child_addr.sin_addr.s_addr = htonl(INADDR_ANY );
            child_addr.sin_port = htons(serverPort);
            cout << "Child Socket Created" << endl;
            logFile << "Child Socket Created" << endl;


            if (protocol==1)
            {
                logFile << "INTIALIZING SELECTIVE REPEAT PROTOCOL" << endl;

				int receiveAckThread = pthread_create(&ackThread, NULL, receiveAckSelectiveRepeat, (void *) t);

				int timerThread = pthread_create(&SelectiveThread, NULL,
						selectiveTimer, (void *) t);

				if (receiveAckThread || timerThread) {
					cout<< "error in starting the ack thread and timer Selective"<< endl;
				} else {
					pthread_detach(ackThread);
					gettimeofday(&time1, NULL);
					long t1 = time1.tv_sec;

					sendBySelectiveRepeat(echoBuffer);
					gettimeofday(&time2, NULL);
					long t2 = time2.tv_sec;
					t2 = t2 - t1;
					logFile << "Time Taken is " << t2 << " secs" << endl;
					logFile.close();
					exit(0);
					shutdown(rqst, 2);
				}
            }
            else if(protocol==0)
            {
                logFile << "INTIALIZING STOP AND WAIT PROTOCOL" << endl;
                gettimeofday(&time1, NULL);
                long t1 = time1.tv_sec;
                sendByStopAndWait(echoBuffer);
                gettimeofday(&time2, NULL);
                long t2 = time2.tv_sec;
                t2 = t2 - t1;
                logFile << "Time Taken is " << t2 << " secs" << endl;
                logFile.close();
                exit(0);

            }
            else if(protocol==2)
            {

                logFile << "INTIALIZING GO BACK N PROTOCOL" << endl;

                rc = pthread_create(&ackThread, NULL, receiveAckGBN, (void *) t);

                int rc1 = pthread_create(&SelectiveThread, NULL,
                                         GBNTimer, (void *) t);

                if (rc || rc1)
                {
                    cout<< "error in starting the ack thread and timer Selective"<< endl;
                }
                else
                {
                    pthread_detach(ackThread);
                    gettimeofday(&time1, NULL);
                    long t1 = time1.tv_sec;

                    sendByGBN(echoBuffer);
                    gettimeofday(&time2, NULL);
                    long t2 = time2.tv_sec;
                    t2 = t2 - t1;
                    logFile << "Time Taken is " << t2 << " secs" << endl;
                    logFile.close();
                    exit(0);
                    shutdown(rqst, 2); /* 3,590,313 close the connection */
                }

            }
        close(childSocket);
        }
        else if (pid == -1)
        {
            cout << "error forking the client process" << endl;
        }
    }

close(svc);
}

int prepareFile(char* fileName)
{
    fp = fopen(fileName, "r");
    if (fp == NULL)
    {
        perror("Server- File NOT FOUND 404");
        return -1;
    }
    fseek(fp, 0L, SEEK_END);
    long size = ftell(fp);

    fseek(fp, 0L, SEEK_SET);

    return size;

}
int outOfOrder = 0;

void extractPacket(packet* packet, char buffer[], int buffLength)
{

    //convert char to unsigned char
    uchar b0 = buffer[0];
    uchar b1 = buffer[1];
    uchar b2 = buffer[2];
    uchar b3 = buffer[3];
    uchar b4 = buffer[4];
    uchar b5 = buffer[5];
    uchar b6 = buffer[6];
    uchar b7 = buffer[7];
    //checksum combine first two bytes
    packet->cksum = (b0 << 8) | b1;
    //len combine second two bytes
    packet->len = (b2 << 8) | b3;
    //seq_no combine third four bytes
    packet->seqno = (b4 << 24) | (b5 << 16) | (b6 << 8) | (b7);
    for (int i = 8; i < buffLength; ++i)
    {
        packet->data[i - 8] = buffer[i];
    }

}

void resendGBN()
{
    int sendFlag = 0;
    for(int i=0;i<frames->size();i++){
    sendFlag = sendto(childSocket, frames->at(i), lens->at(i) + 8, 0,
                      (struct sockaddr *) &client_addr, sizeof(client_addr));
    times->at(i)=TIMEOUT;
    packet p;
    extractPacket(&p, frames->at(i), lens->at(i));
    cout << "Resending Timer , RESEND SEQ NUM = " << p.seqno << endl;
    logFile << "Resending PACKET WITH SEQ NUM = " << p.seqno << endl;
    }

}

void *GBNTimer(void *time)
{

    while (true)
    {
        pthread_mutex_lock(&Selectivemutex);
        for (int i = 0; i < times->size(); ++i)
        {
            times->at(i)--;
            if(times->at(i)==0)
            {
                cout<<"TIME OUT PACKET: "<<base+i<<endl;
                logFile<<"++++++++++++TIME OUT DETECTED++++++++++++++"<<endl;
                times->at(i)= TIMEOUT;
                resend(i);
            }
        }

        pthread_mutex_unlock(&Selectivemutex);
    }

}

void *GBNTimer2(void *time)
{

    while (true)
    {
        pthread_mutex_lock(&Selectivemutex);
        for (int i = 0; i < times->size(); ++i)
        {
            times->at(i)--;
        }
            if(times->at(0)==0)
            {
                cout<<" TIME OUT BASE PACKET: "<<base<<endl;
                logFile<<"++++++++++++TIME OUT DETECTED++++++++++++++"<<endl;
                resendGBN();
            }

        pthread_mutex_unlock(&Selectivemutex);
    }

}
void *receiveAckStopAndWait(void *threadid)
{
    int packetCount = ((intptr_t) threadid);
    socklen_t alen = sizeof(client_addr); /* length of address */
    int reclen;
    char Buffer[8];
    cout << "waiting for ack #" << packetCount << endl;
    logFile << "waiting for ack #" << packetCount << endl;
    bool waitEnd = false;
    while (!waitEnd)
    {
        stopAndWaitAcked = false;
        if (reclen = (recvfrom(childSocket, Buffer, 8, 0,
                               (struct sockaddr *) &client_addr, &alen)) < 0)
            perror("recvfrom() failed");

        pthread_mutex_lock(&StopAndWaitmutexTimer);
        ack_packet packet;
        parseAckPacket(Buffer, 8, &packet);
        cout << "ack #" << packet.ackno << " received" << endl;
        logFile << "ack #" << packet.ackno << " received" << endl;
        uint16_t temp;
        if (packetCount == 0)
            temp = 0;
        else
            temp = 1;
        if (packet.ackno != temp)
        {
            cout << "Received is " << packet.ackno
                 << "...out of order ack...wait for right Packet # "
                 << packetCount << endl;

            logFile << "Received is " << packet.ackno
                    << "...out of order ack...wait for right Packet # "
                    << packetCount << endl;
            outOfOrder++;
            pthread_mutex_unlock(&StopAndWaitmutexTimer);
        }
        else
        {
            cout << "killing Timer" << endl;
            waitEnd = true;
            stopAndWaitAcked = true;
        }
    }
    pthread_mutex_unlock(&StopAndWaitmutexTimer);
    pthread_mutex_unlock(&StopAndWaitmutex);
    pthread_exit(NULL);
}
void *receiveAckGBN(void *threadid)
{

    while (1)
    {
        socklen_t alen = sizeof(client_addr); /* length of address */
        int reclen;
        char Buffer[8];
        if (reclen = (recvfrom(childSocket, Buffer, 8, 0,
                               (struct sockaddr *) &client_addr, &alen)) < 0)
            perror("recvfrom() failed");
        ack_packet packet;
        parseAckPacket(Buffer, 8, &packet);
        int frameIndex = packet.ackno - base;
        pthread_mutex_lock(&Selectivemutex);
        cout << "ack " << packet.ackno << " received" << endl;
        logFile << "ACK " << packet.ackno << " received" << endl;
        cout << "FrameIndex : " << frameIndex << endl;

        if (frameIndex < 0 || ackedFrames->size() == 0)
        {
            cout << "dublicate ack is detected ... ignore" << endl;
            logFile << "dublicate ACK is detected ... ignore" << endl;

        }
        else if (ackedFrames->size() != 0
                 && ackedFrames->at(frameIndex) == true)
        {
            cout << "dublicate ack is detected ... ignore" << endl;
            logFile << "dublicate ACK is detected ... ignore" << endl;
        }
        else
        {
            if (curWindowSize*2 < ssth)
            {

                curWindowSize*=2;
                logFile << "INCREASE WIDOW SIZE FROM  " << curWindowSize/2<< " TO " << curWindowSize << endl;
                logFile2<<ssth<<endl;
                logFile3<<curWindowSize<<endl;



            }else if(curWindowSize<maxWindowSize&&curWindowSize>=ssth){
                curWindowSize++;
                logFile << "INCREASE WINDOW SIZE FROM  " << (curWindowSize - 1)<< " TO " << curWindowSize << endl;
                logFile2<<ssth<<endl;
                logFile3<<curWindowSize<<endl;

            }else if(curWindowSize*2>ssth&&curWindowSize<ssth){
                curWindowSize+= (ssth-curWindowSize);
                logFile << "INCREASE WINDOW SIZE FROM  " << (ssth-curWindowSize)<< " TO " << curWindowSize << endl;
                logFile2<<ssth<<endl;
                logFile3<<curWindowSize<<endl;

            }
            cout << "Buffers size = " << ackedFrames->size() << endl;
            ackedFrames->at(frameIndex) = true;
            //TODO stop timer
            times->at(frameIndex) = TIMERSTOP;
            if (ackedFrames->at(0) == true)
            {
                cout<< "-------------- the base packet is acked..sliding the window"<< endl;
                logFile << "the base packet is acked...sliding the window"<< endl;
                for (int i = 0; i <= curr - base; ++i)
                {
                    if (ackedFrames->size() == 0)
                        break;
                    if (ackedFrames->at(0) == true)
                    {
                        frames->erase(frames->begin());
                        ackedFrames->erase(ackedFrames->begin());
                        times->erase(times->begin());
                        lens->erase(lens->begin());
                        ++base;
                    }
                    else
                    {
                        break;
                    }
                }
                cout << "------------  new base = " << base << endl;
                logFile << "new base pointer = " << base << endl;
            }
        }
        pthread_mutex_unlock(&Selectivemutex);
    }
}

void *receiveAckGBN2(void *threadid)
{

    while (1)
    {
        socklen_t alen = sizeof(client_addr); /* length of address */
        int reclen;
        char Buffer[8];
        if (reclen = (recvfrom(childSocket, Buffer, 8, 0,
                               (struct sockaddr *) &client_addr, &alen)) < 0)
            perror("recvfrom() failed");

        ack_packet packet;
        parseAckPacket(Buffer, 8, &packet);

        int frameIndex = packet.ackno - base;
        pthread_mutex_lock(&Selectivemutex);
        cout << "ack " << packet.ackno << " received" << endl;
        logFile << "ACK " << packet.ackno << " received" << endl;
        cout << "FrameIndex : " << frameIndex << endl;

        if (frameIndex < 0 || ackedFrames->size() == 0)
        {
            cout << "dublicate ack is detected ... ignore" << endl;
            logFile << "dublicate ACK is detected ... ignore" << endl;

        }
        else if (ackedFrames->size() != 0
                 && ackedFrames->at(frameIndex) == true)
        {
            cout << "dublicate ack is detected ... ignore" << endl;
            logFile << "dublicate ACK is detected ... ignore" << endl;
        }
        else if (ackedFrames->size() != 0&&frameIndex==0)
        {
            if (curWindowSize*2 < ssth)
            {

                curWindowSize*=2;
                logFile << "INCREASE WINDOW SIZE FROM  " << curWindowSize/2<< " TO " << curWindowSize << endl;
                logFile2<<ssth<<endl;
                logFile3<<curWindowSize<<endl;


            }else if(curWindowSize<maxWindowSize&&curWindowSize>=ssth){
                curWindowSize++;
                logFile << "INCREASE WINDOW SIZE FROM  " << (curWindowSize - 1)<< " TO " << curWindowSize << endl;
                logFile2<<ssth<<endl;
                logFile3<<curWindowSize<<endl;

            }else if(curWindowSize*2>ssth){
                curWindowSize+= (ssth-curWindowSize);
                logFile << "INCREASE WINDOW SIZE FROM  " << (ssth-curWindowSize)<< " TO " << curWindowSize << endl;
				logFile2<<ssth<<endl;
                logFile3<<curWindowSize<<endl;
            }
            cout << "Buffers size = " << ackedFrames->size() << "time size = "<<times->size()<<endl;
            ackedFrames->at(frameIndex) = true;
            if(times->size()>0)
            times->at(frameIndex) = TIMERSTOP;
            if (ackedFrames->at(0) == true)
            {
                logFile << "the base packet is acked...sliding the window"<< endl;
                        frames->erase(frames->begin());
                        ackedFrames->erase(ackedFrames->begin());
                        times->erase(times->begin());
                        lens->erase(lens->begin());
                        ++base;
                logFile << "new base pointer = " << base << endl;
            }
        }
        pthread_mutex_unlock(&Selectivemutex);
    }
}

void sendByGBN(char* fileName)
{

    int size = prepareFile(fileName);
    packet currPacket;

    cout << "File Size : " << size << endl;
    logFile<<"File Size : " << size << endl;
    int packetCount = 0;

    for (int i = 0; i < size;)
    {

        dropCount++;
        dropCount = dropCount % 101;


        currPacket.len = fread(currPacket.data, 1, 500, fp);
        string st(currPacket.data, strlen(currPacket.data));
        vector<char> bytes(st.begin(), st.end());
        bytes.push_back('\0');
        char *c = &bytes[0];
        currPacket.cksum = (uint16_t)checksum(c);
        logFile<<"check sum = "<<currPacket.cksum<<endl;
        currPacket.seqno = packetCount;
        i += currPacket.len;
        char *buffer = new char[512];
        unsigned int sendFlag = 1;
        convertDataPacketToByte(&currPacket, buffer);

        while (1)
        {
            pthread_mutex_lock(&Selectivemutex);
            if (curr - base < curWindowSize - 1)
            {
                if (!isdropped())
                {
                    cout << "SENDING... current = " << curr << " packet = "
                         << packetCount << " And Window size = " << curWindowSize
                         << endl;
                    sendFlag = sendto(childSocket, buffer, currPacket.len + 8,
                                      0, (struct sockaddr *) &client_addr,
                                      sizeof(client_addr));
                    if (sendFlag < -1)
                        break;
                    else if (sendFlag == -1)
                        perror("sendto");
                }
                else
                {
                    ssth= curWindowSize/2;
                    curWindowSize = 1;
                    logFile2<<ssth<<endl;
                	logFile3<<curWindowSize<<endl;

                    logFile<<"there is a packet loss and the new Window size = "<<curWindowSize<<"and the ssth = "<<ssth<<endl;
                    logFile << endl;
                    logFile << "PACKET :" << packetCount<< " IS DROPPED " << endl;
                    logFile << endl;
                    break;
                }
            }
            else
            {
                pthread_mutex_unlock(&Selectivemutex);
            }
        }
        logFile << "packet number:  " << currPacket.seqno << " is sent" << endl;

        curr = currPacket.seqno;
        frames->push_back(buffer);
        ackedFrames->push_back(false);
        times->push_back(TIMEOUT);
        lens->push_back(currPacket.len);

        pthread_mutex_unlock(&Selectivemutex);
        packetCount++;
    }
    currPacket.cksum = 1;
    currPacket.len = 150;
    currPacket.seqno = packetCount;
    cout << "******		Sending finish packet." << endl;
    logFile << "******		Sending finish packet." << endl;

    gettimeofday(&time2, NULL);
    long t2 = time2.tv_sec;
    t2 = t2 - time1.tv_sec;
    logFile << "Time Taken is " << t2 << " secs" << endl;
    logFile.close();
    char *dataFinish = new char[8];
    int sendFlag = 1;
    convertDataPacketToByte(&currPacket, dataFinish);
    while (1)
    {
        if (curr - base < curWindowSize - 1)
        {
            sendFlag = sendto(childSocket, dataFinish, sizeof(dataFinish), 0,
                              (struct sockaddr *) &client_addr, sizeof(client_addr));
            break;
        }
    }
    pthread_mutex_lock(&Selectivemutex);

    curr = currPacket.seqno;
    frames->push_back(dataFinish);
    ackedFrames->push_back(false);
    lens->push_back(currPacket.len);
    times->push_back(TIMEOUT);
    pthread_mutex_lock(&Selectivemutex);

    if (sendFlag == -1)
        perror("sendto");
    cout << "******		Sending finish packet DONE." << endl;
    cout << "******		Finish Window size =" << curWindowSize << endl;
    logFile << "******		Finish Window size =" << curWindowSize << endl;

}

void sendByGBN2(char* fileName)
{

    int size = prepareFile(fileName);
    packet currPacket;
    cout << "File Size : " << size << endl;
    logFile << "File Size : " << size << endl;
    int packetCount = 0;

    for (int i = 0; i < size;)
    {

        dropCount++;
        dropCount = dropCount % 101;
        currPacket.cksum = 1;
        currPacket.len = fread(currPacket.data, 1, 500, fp);
        currPacket.seqno = packetCount;
        i += currPacket.len;
        char *buffer = new char[512];
        unsigned int sendFlag = 1;
        convertDataPacketToByte(&currPacket, buffer);

        while (1)
        {
            pthread_mutex_lock(&Selectivemutex);
            if (curr - base < curWindowSize - 1)
            {
                if (!isdropped())
                {
                    cout << "SENDING... current = " << curr << " packet = "
                         << packetCount << " And Window size = " << curWindowSize
                         << endl;
                    sendFlag = sendto(childSocket, buffer, currPacket.len + 8,
                                      0, (struct sockaddr *) &client_addr,
                                      sizeof(client_addr));
                    if (sendFlag < -1)
                        break;
                    else if (sendFlag == -1)
                        perror("sendto");
                }
                else
                {
                    ssth= curWindowSize/2;
                    curWindowSize = 1;
                    logFile2<<ssth<<endl;
                	logFile3<<curWindowSize<<endl;
                    logFile<<"there is a packet loss and the new Window size = "<<curWindowSize<<"and the ssth = "<<ssth<<endl;

                    logFile << endl;
                    logFile << "PACKET :" << packetCount<< " IS DROPPED." << endl;
                    logFile << endl;
                    break;
                }
            }
            else
            {
                pthread_mutex_unlock(&Selectivemutex);
            }
        }
        logFile << "packet number:  " << currPacket.seqno << " is sent" << endl;
        curr = currPacket.seqno;
        frames->push_back(buffer);
        ackedFrames->push_back(false);
        times->push_back(TIMEOUT);
        lens->push_back(currPacket.len);

        pthread_mutex_unlock(&Selectivemutex);
        packetCount++;
    }
    currPacket.cksum = 1;
    currPacket.len = 150;
    currPacket.seqno = packetCount;
    cout << "******		Sending finish packet." << endl;
    logFile << "******		Sending finish packet." << endl;

    gettimeofday(&time2, NULL);
    long t2 = time2.tv_sec;
    t2 = t2 - time1.tv_sec;
    logFile << "Time Taken is " << t2 << " secs" << endl;
    logFile.close();
    char *dataFinish = new char[8];
    int sendFlag = 1;
    convertDataPacketToByte(&currPacket, dataFinish);
    while (1)
    {
        if (curr - base < curWindowSize - 1)
        {
            sendFlag = sendto(childSocket, dataFinish, sizeof(dataFinish), 0,
                              (struct sockaddr *) &client_addr, sizeof(client_addr));
            break;
        }
    }
    pthread_mutex_lock(&Selectivemutex);

    curr = currPacket.seqno;
    frames->push_back(dataFinish);
    ackedFrames->push_back(false);
    lens->push_back(currPacket.len);
    times->push_back(TIMEOUT);
    pthread_mutex_lock(&Selectivemutex);

    if (sendFlag == -1)
        perror("sendto");
    cout << "******		Sending finish packet DONE." << endl;
    cout << "******		Finish Window size =" << curWindowSize << endl;
    logFile << "******		Finish Window size =" << curWindowSize << endl;
}


void *StopAndWaitTimer(void *time)
{

    int sleeptime = 3 * 100000;
    int timeHolder = sleeptime;
    while (true)
    {
        pthread_mutex_lock(&StopAndWaitRecThreadCreationMutex);
        pthread_mutex_lock(&StopAndWaitmutexTimer);
        sleeptime--;
        if (stopAndWaitAcked == true)
        {
            sleeptime = timeHolder;
        }
        else if (sleeptime <= 0)
        {
            sleeptime = timeHolder;
            timeCount++;
            cout << "-----------------TIMEOUT-------------------------" << endl;
            logFile << "-----------------TIME OUT-------------------------"
                    << endl;
            pthread_cancel(recThread);
            stopAndWaitAcked = false;
            pthread_mutex_unlock(&StopAndWaitmutex);
        }
        pthread_mutex_unlock(&StopAndWaitmutexTimer);
        pthread_mutex_unlock(&StopAndWaitRecThreadCreationMutex);
    }

}
void sendByStopAndWait(char* fileName)
{
    bool recThreadIsCreated = false;
    bool timerStarted = false;
    int size = prepareFile(fileName);
    packet currPacket;

    cout << "File Size : " << size << endl;
    logFile << "File Size : " << size << endl;
    int packetCount = 0;

    int couny = 0;
    dropCount = 0;
    for (int i = 0; i < size;)
    {
        dropCount++;
        dropCount = dropCount % 101;
        currPacket.len = fread(currPacket.data, 1, 500, fp);
        string st = (string)currPacket.data;
        vector<char> bytes(st.begin(), st.end());
        bytes.push_back('\0');
        char *c = &bytes[0];
        currPacket.cksum = (uint16_t)calculateChecksum(c);
        logFile<<"check sum = "<<currPacket.cksum<<endl;
        currPacket.seqno = packetCount;
        char buffer[512];
        i += currPacket.len;
        unsigned int sendFlag = 1;
        convertDataPacketToByte(&currPacket, buffer);
        bool acked = false;
        stopAndWaitAcked = false;
        while (!acked)
        {
            if (!isdropped())
            {
                while (1)
                {
                    sendFlag = sendto(childSocket, buffer, currPacket.len + 8,
                                      0, (struct sockaddr *) &client_addr,
                                      sizeof(client_addr));
                    if (sendFlag == 0)
                    {
                        break;
                    }
                    else if (sendFlag < -1)
                        break;
                    else if (sendFlag == -1)
                        cout<<"error sendto"<<endl;
                }
                cout << "******		" << packetCount << " Packet Sent" << endl;
                logFile << "PACKET: " << packetCount << " Sent" << endl;
            }
            else
            {
                cout << "Package Dropped" << endl;
                logFile << "----------------Package Dropped---------------"<< endl;
            }
            pthread_mutex_lock(&StopAndWaitmutex);
            pthread_mutex_lock(&StopAndWaitRecThreadCreationMutex);
            if (recThreadIsCreated == true)
            {
                pthread_kill(recThread, 0);
                recThreadIsCreated = false;
            }
            while (1)
            {
                int rc = pthread_create(&recThread, NULL, receiveAckStopAndWait,
                                        (void *) packetCount);
                recThreadIsCreated = true;
                pthread_detach(recThread);
                if (rc)
                {
                    cout << "error in starting the Stop and wait ack thread"
                         << endl;
                    sleep(1);
                }
                else
                {
                    break;
                }
            }
            pthread_mutex_unlock(&StopAndWaitRecThreadCreationMutex);
            if (!timerStarted)
            {
                timerStarted = true;
                int timeout = 3;
                while (1)
                {
                    int rc2 = pthread_create(&stopAndWaitTimerThread, NULL,
                                             StopAndWaitTimer, (void *) timeout);
                    if (rc2)
                    {
                    }
                    else
                    {
                        pthread_detach(stopAndWaitTimerThread);
                        break;
                    }
                }
            }

            pthread_mutex_lock(&StopAndWaitmutex);
            pthread_mutex_lock(&StopAndWaitmutexTimer);
            if (stopAndWaitAcked == true)
            {
                stopAndWaitAcked = false;
                acked = true;
            }
            else
            {
                acked = false;
            }
            pthread_mutex_unlock(&StopAndWaitmutex);
            pthread_mutex_unlock(&StopAndWaitmutexTimer);

        }

        packetCount++;
        packetCount = packetCount % 2;

    }

    currPacket.cksum = 1;
    currPacket.len = 150;
    currPacket.seqno = packetCount;
    cout << "******		Sending finish packet." << endl;
    char dataFinish[8];
    int sendFlag = 1;
    convertDataPacketToByte(&currPacket, dataFinish);
    sendFlag = sendto(childSocket, dataFinish, sizeof(dataFinish), 0,
                      (struct sockaddr *) &client_addr, sizeof(client_addr));

    if (sendFlag == -1)
        perror("sendto");
    pthread_kill(stopAndWaitTimerThread, 0);
    cout << "******		Sending finish packet DONE." << endl;
    cout << "Out Of order Count = " << outOfOrder << endl;
}

void convertDataPacketToByte(packet *packet, char* buffer)
{
    //chksum

    buffer[0] = packet->cksum >> 8;
    buffer[1] = (packet->cksum) & 255;

    //len field

    buffer[2] = packet->len >> 8;
    buffer[3] = (packet->len) & 255;

    //seqnumber

    buffer[4] = packet->seqno >> 24;
    buffer[5] = (packet->seqno >> 16) & 255;
    buffer[6] = (packet->seqno >> 8) & 255;
    buffer[7] = (packet->seqno) & 255;

    //data

    for (int i = 8; i < packet->len + 8; ++i)
    {
        buffer[i] = packet->data[i - 8];
    }

}
void parseAckPacket(char *buffer, int buffLength, ack_packet* packet)
{
    //convert char to unsigned char
    uchar b0 = buffer[0];
    uchar b1 = buffer[1];
    uchar b2 = buffer[2];
    uchar b3 = buffer[3];
    uchar b4 = buffer[4];
    uchar b5 = buffer[5];
    uchar b6 = buffer[6];
    uchar b7 = buffer[7];
    //checksum combine first two bytes
    packet->cksum = (b0 << 8) | b1;
    //len combine second two bytes
    packet->len = (b2 << 8) | b3;
    //seq_no combine third four bytes
    packet->ackno = (b4 << 24) | (b5 << 16) | (b6 << 8) | (b7);
}

void sig_func(int sig)
{
    cout << "receiving thread is dead" << endl;
    pthread_exit(NULL);
}
