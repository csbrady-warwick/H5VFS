.PHONY: all clean

SRC_DIR = src
OBJ_DIR = obj
BIN_DIR = bin
INC_DIR = include

SRCS = $(SRC_DIR)/toHDF5.cpp $(SRC_DIR)/h5vfs.cpp
OBJS = $(OBJ_DIR)/toHDF5.o $(OBJ_DIR)/h5vfs.o
BINS = $(BIN_DIR)/toHDF5 $(BIN_DIR)/h5vfs

FUSELIBS = `pkg-config fuse --cflags --libs`

all: $(BINS)

$(BIN_DIR)/toHDF5: $(OBJ_DIR)/toHDF5.o
	mkdir -p $(BIN_DIR)
	h5c++ -g -O3 -I $(INC_DIR) -o $(BIN_DIR)/toHDF5 $(OBJ_DIR)/toHDF5.o

$(BIN_DIR)/h5vfs: $(OBJ_DIR)/h5vfs.o
	mkdir -p $(BIN_DIR)
	h5c++ -g -O3 -I $(INC_DIR) -o $(BIN_DIR)/h5vfs $(OBJ_DIR)/h5vfs.o $(FUSELIBS)

$(OBJ_DIR)/toHDF5.o: $(SRC_DIR)/toHDF5.cpp
	mkdir -p $(OBJ_DIR)
	h5c++ -g -O3 -I $(INC_DIR) -c $(SRC_DIR)/toHDF5.cpp -o $(OBJ_DIR)/toHDF5.o

$(OBJ_DIR)/h5vfs.o: $(SRC_DIR)/h5vfs.cpp
	mkdir -p $(OBJ_DIR)
	h5c++ -g -O3 -I $(INC_DIR) -c $(SRC_DIR)/h5vfs.cpp -o $(OBJ_DIR)/h5vfs.o $(FUSELIBS)

clean:
	rm -rf $(BIN_DIR) $(OBJ_DIR)
