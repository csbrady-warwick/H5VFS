#ifndef COMMANDLINEMODIFIER_H
#define COMMANDLINEMODIFIER_H

#include <stdexcept>

  class CommandlineModifier {
    int argc;
    char** argv;

    int argcMod;
    char** argvMod=nullptr;
    bool* argvModDelete=nullptr;

    void copySlots(){
        cleanupSlots();
        argcMod = argc;
        argvMod = new char*[argcMod+1];
        argvModDelete = new bool[argcMod];
        for (int i=0;i<argcMod;i++){
            argvMod[i]=argv[i];
            argvModDelete[i]=false;
        }
        argvMod[argcMod]=nullptr;
    }

    void cleanupSlots(){
        if (argvMod){
            for (int i=0;i<argcMod;i++){
                if (argvModDelete[i]){
                    delete[] argvMod[i];
                }
            }
            delete[] argvMod;
        }
        if (argvModDelete)
            delete[] argvModDelete;
    }

    void addSlotAt(int index){
        if (index>argcMod || index<0){
            throw std::runtime_error("Index out of bounds");
        }
        argcMod++;
        char **argvModTemp = new char*[argcMod+1];
        bool *argvModDeleteTemp = new bool[argcMod];

        for (int i=0;i<std::min(index,argcMod-1);i++){
            argvModTemp[i]=argvMod[i];
            argvModDeleteTemp[i]=argvModDelete[i];
        }
        for (int i=index+1;i<argcMod;i++){
            argvModTemp[i]=argvMod[i-1];
            argvModDeleteTemp[i]=argvModDelete[i-1];
        }
        argvModTemp[index]=nullptr;
        argvModDeleteTemp[index]=false;
        argvModTemp[argcMod]=nullptr;
        delete[] argvModDelete;
        delete[] argvMod;
        argvMod=argvModTemp;
        argvModDelete=argvModDeleteTemp;
    }

    void deleteSlot(int index){
        if (index<0 || index>=argcMod){
            throw std::runtime_error("Index out of bounds");
        }
        argcMod--;
        char **argvModTemp = new char*[argcMod+1];
        bool *argvModDeleteTemp = new bool[argcMod];

        for (int i=0;i<index;i++){
            argvModTemp[i]=argvMod[i];
            argvModDeleteTemp[i]=argvModDelete[i];
        }
        for (int i=index;i<argcMod;i++){
            argvModTemp[i]=argvMod[i+1];
            argvModDeleteTemp[i]=argvModDelete[i+1];
        }
        delete[] argvModDelete;
        delete[] argvMod;
        argvMod=argvModTemp;
        argvMod[argcMod]=nullptr;
        argvModDelete=argvModDeleteTemp;
    }

    public:

    CommandlineModifier(int argc, char** argv) : argc(argc), argv(argv) {
        //Initial state is a copy of the original commandline
        copySlots();
    }

    ~CommandlineModifier() {
        cleanupSlots();
    }

    void deleteArgument(int index) {
        deleteSlot(index);
    }

    void addArgument(int at, const std::string &arg) {
        addSlotAt(at);
        argvMod[at] = new char[arg.size() + 1];
        argvModDelete[at] = true;
        std::copy(arg.begin(), arg.end(), argvMod[at]);
        argvMod[at][arg.size()] = '\0';
    }

    void addArgument(const std::string &arg) {
        addSlotAt(argcMod);
        argvMod[argcMod-1] = new char[arg.size() + 1];
        argvModDelete[argcMod-1] = true;
        std::copy(arg.begin(), arg.end(), argvMod[argcMod-1]);
        argvMod[argcMod-1][arg.size()] = '\0';
    }

    void changeArgument(int index, const std::string &arg) {
        if (index<0 || index>=argcMod){
            throw std::runtime_error("Index out of bounds");
        }
        if (argvModDelete[index]){
            delete[] argvMod[index];
        }
        argvMod[index] = new char[arg.size() + 1];
        argvModDelete[index] = true;
        std::copy(arg.begin(), arg.end(), argvMod[index]);
        argvMod[index][arg.size()] = '\0';
    }

    char *operator[](int index) {
        if (index<0 || index>=argcMod){
            throw std::runtime_error("Index out of bounds");
        }
        return argvMod[index];
    }

    char** getArgv() {
        return argvMod;
    }

    int getArgc() {
        return argcMod;
    }
  };

#endif