#define FUSE_USE_VERSION 26
#include <fuse.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <iostream>
#include <string>
#include <map>
#include <mutex> 
#include <fstream>
#include <ios>
#include <filesystem>
//Include the HDF5 library
#include <H5Cpp.h>
#include "modifier.h"

#define ATTR_FLAG ".attr."

std::string mountedFile;
std::string mountPoint;
H5::H5File mainfile;
//Store the time that the HDF5 file was last modified
//This variable is the one used by the fuse functions
time_t lastModified;
std::recursive_mutex mtx;
bool showAttributesAsFiles = true;

size_t getDatasetSize(H5::DataSet dataset) {
    H5::DataSpace dataspace = dataset.getSpace();
    //Get the rank of the dataset
    int rank = dataspace.getSimpleExtentNdims();
    hsize_t allDims [H5S_MAX_RANK];
    //Get the dimensions of the dataset
    dataspace.getSimpleExtentDims(allDims, NULL);
    size_t dim = 1;
    for (int i = 0; i < rank; i++) {
        dim *= allDims[i];
    }
    //Multiply by the size in bytes of the datatype
    H5::DataType type = dataset.getDataType();
    dim *= type.getSize();
    return dim;
}

std::string getLastPart(std::string path) {
    if (path == "/") return path;
    if (path[path.size() - 1] == '/') {
        path = path.substr(0, path.size() - 1);
    }
    size_t pos = path.find_last_of("/");
    //Return from pos + 1 to the end of the string
    return path.substr(pos + 1);
}

std::string getPrefix(std::string path) {
    if (path == "/") return path;
    if (path[path.size() - 1] == '/') {
        path = path.substr(0, path.size() - 1);
    }
    size_t pos = path.find_last_of("/");
    //Return from the start of the string to pos
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

bool isNameAttribute(std::string name, H5::Attribute &attr) {
    size_t pos = name.find(ATTR_FLAG);
    if (pos == std::string::npos) return false;
    std::string prefix = getPrefix(name);
    std::string lastPart = getLastPart(name);

    size_t attrflagpos = lastPart.find(ATTR_FLAG);
    if (attrflagpos == std::string::npos) return false;
    if (!mainfile.nameExists(prefix)) return false;
    H5::Group group = mainfile.openGroup(prefix);
    std::string dataset = lastPart.substr(1, attrflagpos-1);
    std::string attribute = lastPart.substr(attrflagpos + strlen(ATTR_FLAG));
    if (dataset == "" || attribute == "") return false;
    if (!group.nameExists(dataset)) return false;
    //Check if dataset is a dataset or another group
    //Either way, check for the attribute
    if (group.childObjType(dataset) == H5O_TYPE_GROUP) {
        H5::Group subgroup = group.openGroup(dataset.c_str());
        if (subgroup.attrExists(attribute)){
            attr = subgroup.openAttribute(attribute);
            return true;
        }
    } else if (group.childObjType(dataset) == H5O_TYPE_DATASET) {
        H5::DataSet ds = group.openDataSet(dataset.c_str());
        if (ds.attrExists(attribute)){
            attr = ds.openAttribute(attribute);
            return true;
        }
    }
    return false;
}

struct h5vfsFile {
    H5::DataSet dataset;
    hsize_t dim[1];
    bool isOpen;
    size_t refcount;
    char *buffer=nullptr;
    h5vfsFile() : refcount(0), isOpen(false) {}
    void open (std::string path) {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        //Because we have these in a map
        //They will never be opened with a different path
        //So if they are already open, we don't need to do anything
        refcount++;
        if (isOpen) return;
        dataset = mainfile.openDataSet(path);
        dim[0] = getDatasetSize(dataset);
        isOpen=true;
    }
    void close() {
        std::lock_guard<std::recursive_mutex> lock(mtx);
        refcount--;
        if (refcount == 0) {
            isOpen=false;
            dataset.close();
            if (buffer) {
                delete[] buffer;
                buffer=nullptr;
            }
        }
    }
    ~h5vfsFile(){
        refcount=0;
        close();
    }
};

std::map<std::string, h5vfsFile> openFiles;

uint8_t *buffer=nullptr;
size_t buffer_size=0;

// Function to get file attributes
static int h5vfs_getattr(const char *path, struct stat *stbuf) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    memset(stbuf, 0, sizeof(struct stat));
    //Get the users username and primary group
    auto uid = getuid();
    auto gid = getgid();
    //Set the user and group to the current user
    stbuf->st_uid = uid;
    stbuf->st_gid = gid;
    //Set created and modified times to the last modified time of the file
    stbuf->st_ctime = lastModified;
    stbuf->st_mtime = lastModified;
    //Note that these will be overwritten if the group or dataset has the attributes "Created" and "Modified"
    //Deal with . and .. first
    if (strcmp(path, ".") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    if (strcmp(path, "..") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    //If the name doesn't exist then it might be 
    //An attribute-as-file
   if (!mainfile.nameExists(path)){
        H5::Attribute attr;
        if (!isNameAttribute(path, attr)) return -ENOENT;
        //Get the size of the attribute
        H5::DataType type = attr.getDataType();
        size_t size = type.getSize();
        //Check if the attribute is a scalar
        int rank = attr.getSpace().getSimpleExtentNdims();
        if (rank != 0){
            //Get the extent of the attribute
            H5::DataSpace space = attr.getSpace();
            hsize_t dims[H5S_MAX_RANK];
            space.getSimpleExtentDims(dims);
            for (int i = 0; i < rank; i++) {
                size *= dims[i];
            }
        }
        //Set the size of the file to the size of the attribute
        stbuf->st_size = size;
        //Set the mode to a file with read permissions, no write permissions
        //Set the execution bit to 0
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        return 0;
   }

    //Next check for links
    if (H5Lexists(mainfile.getId(), path, H5P_DEFAULT)) {
        H5L_info_t info;
        memset(&info, 0, sizeof(H5L_info_t));
        H5Lget_info(mainfile.getId(), path, &info, H5P_DEFAULT);
        if (info.type == H5L_TYPE_SOFT) {
            stbuf->st_mode = S_IFLNK | 0777;
            stbuf->st_nlink = 1;
            //Get the destination of the link
            hsize_t len = info.u.val_size;
            std::string link(len, '\0');
            H5Lget_val(mainfile.getId(), path, &link[0], len, H5P_DEFAULT);
            //Get the type that the link points to
            H5O_type_t c = mainfile.childObjType(link);
            //If the link points to a group then it is a directory, so job done
            if (c == H5O_TYPE_GROUP) {
                return 0;
            }
            //Otherwise it is a file
            else if (c == H5O_TYPE_DATASET) {
                H5::DataSet dataset = mainfile.openDataSet(link);
                stbuf->st_size = getDatasetSize(dataset);
                return 0;
            }
            return -ENOENT;
        }
    }

    H5O_type_t c = mainfile.childObjType(path);
    //If path is to a group then return a directory
    if (c == H5O_TYPE_GROUP) {
        //If a group has the attribute "ExternalLink" then it is a link
        H5::Group group = mainfile.openGroup(path);
        if (group.attrExists("ExternalLink")) {
            stbuf->st_mode = S_IFLNK | 0777;
            stbuf->st_nlink = 1;
            //Get the attribute "ExternalLink"
            H5::Attribute attr = group.openAttribute("ExternalLink");
            std::string link;
            attr.read(attr.getDataType(), link);
            //Get the size of the linked file
            struct stat linkStat;
            memset(&linkStat, 0, sizeof(struct stat));
            stat(link.c_str(), &linkStat);
            stbuf->st_size = linkStat.st_size;
            return 0;
        }

        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        //Get the created and modified times from the group
        //Check if the group has the attribute "Modified"
        if(group.attrExists("Modified")){
            H5::Attribute attr = group.openAttribute("Modified");
            int64_t modified;
            attr.read(H5::PredType::NATIVE_INT64, &modified);
            stbuf->st_mtime = modified;
        }
        //Check if the group has the attribute "Created"
        if(group.attrExists("Created")){
            H5::Attribute attr = group.openAttribute("Created");
            int64_t created;
            attr.read(H5::PredType::NATIVE_INT64, &created);
            stbuf->st_ctime = created;
        }
        //Check for the attribute "permissions"
        if(group.attrExists("Permissions")){
            H5::Attribute attr = group.openAttribute("Permissions");
            int64_t permissions;
            attr.read(H5::PredType::NATIVE_INT64, &permissions);
            stbuf->st_mode = S_IFDIR | permissions;
        }
        return 0;
    }
    //Otherwise return a file
    else if (c == H5O_TYPE_DATASET) {
        //Set the mode to a file with read permissions, no write permissions
        //Set the execution bit to 1
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        //Get the size of the file. All datasets are 1D arrays of uint8
        H5::DataSet dataset = mainfile.openDataSet(path);
        stbuf->st_size = getDatasetSize(dataset);
        if(dataset.attrExists("Modified")){
            H5::Attribute attr = dataset.openAttribute("Modified");
            int64_t modified;
            attr.read(H5::PredType::NATIVE_INT64, &modified);
            stbuf->st_mtime = modified;
        }
        //Check if the group has the attribute "Created"
        if(dataset.attrExists("Created")){
            H5::Attribute attr = dataset.openAttribute("Created");
            int64_t created;
            attr.read(H5::PredType::NATIVE_INT64, &created);
            stbuf->st_ctime = created;
        }
        //Check for the attribute "permissions"
        if(dataset.attrExists("Permissions")){
            H5::Attribute attr = dataset.openAttribute("Permissions");
            int64_t permissions;
            attr.read(H5::PredType::NATIVE_INT64, &permissions);
            stbuf->st_mode = S_IFREG | permissions;
        }
        return 0;
    }
    return -ENOENT;
}

static int h5vfs_readlink(const char *path, char *buf, size_t size) {
    std::lock_guard<std::recursive_mutex> lock(mtx);

    //First check if the path is actually a group
    //if it is then it is an external link
    std::cout << "Checking for external link: " << path << std::endl;
    if (mainfile.childObjType(path) == H5O_TYPE_GROUP) {
        H5::Group group = mainfile.openGroup(path);
        if (group.attrExists("ExternalLink")) {
            H5::Attribute attr = group.openAttribute("ExternalLink");
            //Get the attribute Size
            H5::DataType type = attr.getDataType();
            size_t attrsize = type.getSize();
            std::string link(attrsize, '\0');
            attr.read(type, &link[0]);
            if (link.size() >= size) return -ENAMETOOLONG;
            memcpy(buf, link.c_str(), link.size()+1);
            return 0;
        }
    }

    //Get the length of the link
    H5L_info_t info;
    H5Lget_info(mainfile.getId(), path, &info, H5P_DEFAULT);
    if (info.type != H5L_TYPE_SOFT) return -ENOENT;
    hsize_t len = info.u.val_size;
    if (len == 0) return -ENOENT;
    std::string link(len, '\0');
    //Get the link
    H5Lget_val(mainfile.getId(), path, &link[0], len, H5P_DEFAULT);
    //Links will be relative to the root of the HDF5 file
    //Convert that to a path relative to the mount point
    std::string fullpath = mountPoint + link;
    std::cout << "Link: " << fullpath << std::endl;
    if (fullpath.size() >= size) return -ENAMETOOLONG;
    memcpy(buf, fullpath.c_str(), fullpath.size()+1);
    return 0;
}

// Function to read directory
static int h5vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
   //Use the path to get the group
   if (!mainfile.nameExists(path)) 
       return -ENOENT;
    //Add . and .. to the directory listing
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    H5::Group group = mainfile.openGroup(path);
    //Convert the contents of the group to the directory listing
    for (int i = 0; i < group.getNumObjs(); i++) {
        std::string name = group.getObjnameByIdx(i);
        //Check if the getOffset function returns a value
        //If it does, then it is a dataset that can be read
        if (group.childObjType(name) == H5O_TYPE_GROUP) {
            filler(buf, name.c_str(), NULL, 0);
            if (showAttributesAsFiles){
                //Loop over the attributes of the group
                //and create a file for each one with the name of .filename.attributename
                H5::Group subgroup = group.openGroup(name);
                for (int j = 0; j < subgroup.getNumAttrs(); j++) {
                    H5::Attribute attr = subgroup.openAttribute(j);
                    std::string attrname = "." + name + ATTR_FLAG + attr.getName();
                    filler(buf, attrname.c_str(), NULL, 0);
                }
            }
        } else if (group.childObjType(name) == H5O_TYPE_DATASET) {
            filler(buf, name.c_str(), NULL, 0);
            if (showAttributesAsFiles){
                //Loop over the attributes of the dataset
                //and create a file for each one with the name of .filename.attributename
                H5::DataSet dataset = group.openDataSet(name);
                for (int j = 0; j < dataset.getNumAttrs(); j++) {
                    H5::Attribute attr = dataset.openAttribute(j);
                    std::string attrname = "." + name + ATTR_FLAG + attr.getName();
                    filler(buf, attrname.c_str(), NULL, 0);
                }
            }
        } else if (group.childObjType(name) == H5O_TYPE_UNKNOWN) {
            //If the object is a link, then add it to the directory listing
            filler(buf, name.c_str(), NULL, 0);
        }
    }
    return 0;
}

// Function to open a file
static int h5vfs_open(const char *path, struct fuse_file_info *fi) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    //Check if the file exists and is a dataset
    if (!mainfile.nameExists(path)) {
        H5::Attribute attr;
        //Don't actually worry about the attribute, just check if it exists
        if (!isNameAttribute(path, attr)) return -ENOENT;
        return 0;
    }
    
    h5vfsFile& file = openFiles[path];
    file.open(path);
    return 0;
}

// Function to read a file
static int h5vfs_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    //If no file is open, return an error
    if (openFiles.find(path) == openFiles.end()) {
        H5::Attribute attr;
        if (!isNameAttribute(path, attr)) return -ENOENT;
        //File is an attribute
        H5::DataType type = attr.getDataType();
        size_t attrsize = type.getSize();
        //Get the extent of the attribute
        H5::DataSpace space = attr.getSpace();
        hsize_t dims[1];
        space.getSimpleExtentDims(dims);
        //If the attribute is a scalar, then the size is the size of the attribute
        attrsize *= dims[0];
        //If the offset is greater than the size of the attribute, return 0
        if (offset >= attrsize) return 0;
        //If the offset plus the size is greater than the size of the attribute, set the size to the size of the attribute minus the offset
        if (offset + size > attrsize) size = attrsize - offset;
        //Create a buffer to read the attribute into
        uint8_t *buffer = new uint8_t[attrsize];
        //Read the attribute
        attr.read(type, buffer);
        //Copy the attribute into the buffer
        memcpy(buf, buffer + offset, size);
        delete[] buffer;
        return size;
    }
    h5vfsFile& file = openFiles[path];
    //If the offset is greater than the size of the file, return 0
    if (offset >= file.dim[0]) return 0;
    //If the offset plus the size is greater than the size of the file, set the size to the size of the file minus the offset
    if (offset + size > file.dim[0]) size = file.dim[0] - offset;

    //Already got the file loaded into memory
    if (file.buffer){
        memcpy(buf, file.buffer + offset, size);
        return size;
    }

    //Get the file offset for the dataset
    haddr_t fileOffset;
    try {
        fileOffset = file.dataset.getOffset();
        //This is fairly horrible, but it is the only way to get data from an
        //arbitrary HDF5 dataset
        std::fstream filestream(mountedFile, std::ios::in | std::ios::binary);
        filestream.seekg(fileOffset + offset);
        filestream.read(buf, size);
        filestream.close();
    } catch (H5::Exception e) {
        //If the file is > rank 1 then have to load the whole thing into memory
        //if (file.dataset.getSpace().getSimpleExtentNdims() > 1) {
            file.buffer = new char[file.dim[0]];
            file.dataset.read(file.buffer, file.dataset.getDataType());
            memcpy(buf, file.buffer + offset, size);
/*        } else {
            //Otherwise, can with care read it with a hyperslab
            hsize_t h5size = size;
            H5::DataSpace memspace(1, &h5size);
            H5::DataSpace filespace = file.dataset.getSpace();
            //Get the type of the dataset
            H5::DataType type = file.dataset.getDataType();
            hsize_t elementSize = type.getSize();
            hsize_t h5offset = offset / elementSize;
            h5size = size / elementSize + 1;
            char *tbuf = new char[h5size * elementSize];
            filespace.selectHyperslab(H5S_SELECT_SET, &h5size, &h5offset);
            file.dataset.read(tbuf, type, memspace, filespace);
            //Now copy to the real buffer. Remember that you
            //may have to shift the start since the offset
            //is in bytes, but the hyperslab is in elements
            size_t realLB = offset - h5offset * elementSize;
            memcpy(buf, tbuf + realLB, size);
            delete[] tbuf;
        }*/
    }

    return size;
}

// Function to release a file
static int h5vfs_release(const char *path, struct fuse_file_info *fi) {
    std::lock_guard<std::recursive_mutex> lock(mtx);
    //If the file is not open, return an error
    if (openFiles.find(path) == openFiles.end()) return -ENOENT;
    h5vfsFile& file = openFiles[path];
    file.close();
    if (file.refcount == 0) {
        openFiles.erase(path);
    }
    return 0;
}

static struct fuse_operations h5vfs_oper = {
    .getattr = h5vfs_getattr, //Line 95
    .readlink = h5vfs_readlink, //Line 105
    .open = h5vfs_open, //Line 173
    .read = h5vfs_read, // Line 186
    .release = h5vfs_release, //Line 200
    .readdir = h5vfs_readdir, //Line 304
};

int main(int argc, char *argv[]) {
    //First parameter is the file to mount, second is the mount point
    //Strip out the first parameter and pass the rest to fuse_main
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <image> <mountpoint> {options}\n", argv[0]);
        return 1;
    }

    CommandlineModifier clmod(argc, argv);
    char path[PATH_MAX];
    mountedFile = realpath(clmod[1], path);
    mountPoint = realpath(clmod[2], path); 

    //Check if the HDF5 file exists
    if (access(mountedFile.c_str(), F_OK) == -1) {
        fprintf(stderr, "File %s does not exist\n", mountedFile.c_str());
        return 1;
    }

    //Get the modification time of the file
    struct stat fileStat;
    stat(mountedFile.c_str(), &fileStat);
    lastModified = fileStat.st_mtime;

    //Open the HDF5 file
    mainfile = H5::H5File(mountedFile, H5F_ACC_RDONLY);
    //If this is an H5VFS file, then the root group will have the attribute "H5VFS"
    //If this is the case then don't show attributes as files
    if (mainfile.attrExists("H5VFS")) {
        showAttributesAsFiles = false;
    }

    //Remove the file from the arguments
    clmod.deleteArgument(1);
    //After all of the other arguments, add "-ofsname=h5vfs" and "-oro"
    clmod.addArgument("-ofsname=h5vfs");
    clmod.addArgument("-oro");

    for (int i = 0; i < clmod.getArgc(); i++) {
        std::cout << clmod.getArgv()[i] << " ";
    }

    return fuse_main(clmod.getArgc(), clmod.getArgv(), &h5vfs_oper, NULL);
}
