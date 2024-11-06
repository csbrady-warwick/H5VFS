# H5VFS
A Read Only FUSE virtual file system mounting HDF5 files as directorys and a utility to create an HDF5 file from a directory structure

HDF5 is a library for creating data formats which is very popular in scientific and research computing.
As such, its a good candidate for producing combined data files to avoid the problems of very many small files in some workflows.
This repository uses the FUSE (filesystem in UserspacE) interface to allow one to mount an HDF5 file as though it was a filesystem, plus a tool to create an HDF5 file from a directory structure. These two make changing your workflow simple.

The resulting file-system mount is read-only, because HDF5 doesn't perform very well when mimicing a read-write filesystem.

## How to use these tools

Once built, using the tools adds only 3 easy steps. Assuming you have some data laid out in a directory structure, which your workflow needs to read, just:

- Step 1: create your file
- Step 2: mount the file
- Step 3: do your work
- Step 4: unmount the file when done

### Getting the tools

This tool originates from the SCRTP RSE team at the University of Warwick. The tool is centrally available on SCRTP systems via the module system (`ml spider H5VFS`)

The tool supports Linux and OSX systems (not Windows).

Otherwise, to build the tool you need:
- Install the fuse package AND the fuse devel package if required (search for "install libfuse \<your operating system\>)
  - For instance, on Ubuntu or similar the package is `libfuse-dev`
  - On OSX look for MacFUSE
- Install the HDF5 package (if required) to get the h5c++ compiler
- Clone or download the code from this repo
- In a terminal, run `make`
- Add the resulting `./bin` directory to your PATH to be able to use the tools

### Creating a mountable file

Once the tools are built, and available in your PATH, you should be able to run the commands `h5vfs` and `toHDF5`.

`toHDF5` creates a mountable HDF5 file, where folders are mapped to Groups, and files are Datasets. The basic use of this tools is just: `toHDF5 <dir_name>` which creates a file called dir\_name.h5 containing (recursively) everything in that directory.

More detailled control of what files are included is possible: see `toHDF5 --help` for details.

Once created, you can examine the file with `h5ls -r <filename>` - a tool provided by HDF5 itself to examine files.

### Mounting the file

Mounting the file uses the h5vfs tool.

IMPORTANT NOTE: if you are on a shared system, please think about where you are mounting, because the filesystem may be provided over the network. If you have access to a /tmp directory please use this as the mount point.
This is EXTREMELY important if you are running on Warwick SCRTP shared machines. Mounting into home or storage will thrash the filesystem and this is not kind to anybody.

First, create a folder to mount under. See above about creating this under /tmp on shared systems. Now run `h5vfs <path to hdf5 file> <path to mount point>`. For example `h5vfs ./projectData/sorted-flowers.h5 /tmp/projectData/`. Now the mount point should contain a directory for the top-level Group, and all data below this will show as files and folders, identical to your original structure.

IMPORTANT: while mounted, the file cannot be edited. You need to unmount it, change it, and remount it if you want to add data etc.

### Running your workflow

No changes required! This part just works.

### Unmounting the File

When you are done, you can unmount the file using `umount <path to mount point>`. After this the file is a normal file again.


# Attribution
Tool created by C.S.Brady, Senior Research Software Engineer, Univerity of Warwick to support workflows needing many-file data sets.

