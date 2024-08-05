#include <regex>
#include <iostream>
#include <stdio.h>
#include <fstream>
#include <string>
#include <filesystem>
#include <map>
#include <algorithm>
#include <H5Cpp.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include "picohash.h"

#define VERSION "0.1.0"
#define VERSIONSTRING "toHDF5 version " VERSION

// Some links can't be created in the order they are found, so defer them until the end
std::vector<std::pair<std::string, std::string>> deferredLinks;

// Used to deal with hard links
// Map inodes to the path to the real file
std::map<ino_t, std::string> inoMap;

/**
 * Class for handling command line options
 */
class Opts
{
	std::map<std::string, std::vector<std::string>> params;
	std::map<std::string, bool> multiParams;

public:
/*
 * Add a key to the options
 */
	void addKey(const std::string &key, bool multi = false)
	{
		params[key] = std::vector<std::string>();
		multiParams[key] = multi;
	}
	/*
	 * Parse the command line options
	 */
	void parse(int argc, char **argv)
	{
		for (int i = 1; i < argc; ++i)
		{
			std::string arg = std::string(argv[i]);
			if (arg.find("--") == 0)
			{
				std::string key = arg.substr(2);
				std::string value = "";
				// Split the key (before =) and value (after =)
				auto pos = key.find("=");
				if (pos != std::string::npos)
				{
					value = key.substr(pos + 1);
					key = key.substr(0, pos);
				}
				// Convert the key to lowercase
				std::transform(key.begin(), key.end(), key.begin(), ::tolower);
				// If the value starts and ends with quotes, remove them
				if (value.size() > 1 && value.front() == '"' && value.back() == '"')
				{
					value = value.substr(1, value.size() - 2);
				}
				if (params.find(key) == params.end())
				{
					std::cerr << "Unknown key \"" << key << "\" use --help to see parameters\n";
					exit(-1);
				}
				if (params[key].size() > 1 && !multiParams[key])
				{
					std::cerr << "Multiple values for \"" << key << "\" are not valid. Use --help to see parameters\n";
					exit(-1);
				}
				params[key].push_back(value);
			}
			else
			{
				// This is the directory, so use "path" as the key
				params["path"].push_back(arg);
			}
		}
	}
	// Overload the [] operator to return the vector of values for a key
	std::vector<std::string> &operator[](std::string key) { return params[key]; }
	// Functions to return the value as a specific type
	std::string asString(std::string key) { return params[key][0]; }
	std::string asString(std::string key, std::string def)
	{
		if (params[key].size() != 0)
			return params[key][0];
		return def;
	}
	int64_t asInt(std::string key) { return std::stoll(params[key][0]); }
	int64_t asInt(std::string key, int64_t def)
	{
		if (params[key].size() != 0)
			return std::stoll(params[key][0]);
		return def;
	}
	long double asReal(std::string key) { return std::stold(params[key][0]); }
	long double asReal(std::string key, long double def)
	{
		if (params[key].size() != 0)
			return std::stold(params[key][0]);
		return def;
	}
	bool asBool(std::string key)
	{
		std::string s = params[key][0];
		return s == "" || ::tolower(s[0]) == 't' ? true : false;
	}
	bool asBool(std::string key, bool def)
	{
		if (params[key].size() != 0)
		{
			return asBool(key);
		}
		return def;
	}
	bool present(std::string key) { return params[key].size() != 0; }
};

/*
 * Convert a glob pattern to a regex pattern
 */
std::string globToRegex(const std::string &glob)
{
	std::string regex;
	for (char c : glob)
	{
		switch (c)
		{
		case '*':
			regex += ".*";
			break;
		case '?':
			regex += '.';
			break;
		case '.':
			regex += "\\.";
			break;
		default:
			regex += c;
			break;
		}
	}
	return regex;
}

/*
 * Check if a string matches a regex. If the regex is empty, then return onEmpty
 */
bool matchesRegex(const std::string &str, const std::string &regex, bool onEmpty = true)
{
	if (regex == "")
		return onEmpty;
	std::regex r(regex, std::regex_constants::grep);
	return std::regex_match(str, r);
}

/*
 * Check if a string matches any of a list of regexes. If the list is empty, then return onEmpty
 */
bool matchesRegex(const std::string &str, const std::vector<std::string> &regexes, bool onEmpty = true)
{
	if (regexes.size() == 0)
		return onEmpty;
	for (auto &regex : regexes)
	{
		std::regex r(regex, std::regex_constants::grep);
		if (std::regex_match(str, r))
			return true;
	}
	return false;
}

/*
 * Get the last chunk of a path. If the path ends in a slash, then the penultimate chunk is returned
 */
std::string getLastPathChunk(const std::string &path)
{
	// Find the position of the last slash and the penultimate slash
	size_t lastSlash = path.find_last_of('/');
	// If there is no slash, return the path
	if (lastSlash == std::string::npos)
	{
		return path;
	}
	// If the slash is not the last character, return the substring from the last slash to the end
	if (lastSlash != path.size() - 1)
	{
		return path.substr(lastSlash + 1);
	}
	// If the slash is the last character, find the penultimate slash
	size_t penultimateSlash = path.find_last_of('/', lastSlash - 1);
	// If there is no penultimate slash, return an empty string
	if (penultimateSlash == std::string::npos)
	{
		return "";
	}
	// Return the substring from the penultimate slash to the last slash
	return path.substr(penultimateSlash + 1, lastSlash - penultimateSlash - 1);
}

/*
 * Check if one path is a subpath of another
 */
bool is_subpath(const std::filesystem::path &parent, const std::filesystem::path &child)
{
	// Normalize the paths
	auto parent_path = std::filesystem::weakly_canonical(parent);
	auto child_path = std::filesystem::weakly_canonical(child);

	// Check if parent_path is a prefix of child_path
	return std::mismatch(parent_path.begin(), parent_path.end(), child_path.begin()).first == parent_path.end();
}

/**
 * Create a group in an HDF5 file from a path
 */
H5::Group createHDF5GroupsFromPath(H5::H5File &file, const std::string &path)
{
	H5::Group currentGroup = file.openGroup("/");
	std::stringstream ss(path);
	std::string groupName;

	// Split the path into individual group names
	while (std::getline(ss, groupName, '/'))
	{
		// Check if the group already exists
		if (!currentGroup.nameExists(groupName))
		{
			// Create the group
			try
			{
				currentGroup = currentGroup.createGroup(groupName);
			}
			catch (H5::GroupIException &error)
			{
				std::cerr << "Failed to create group: " << groupName << std::endl;
				throw;
			}
		}
		else
		{
			// Open the existing group
			currentGroup = currentGroup.openGroup(groupName);
		}
	}

	return currentGroup;
}

// class enum for should store, meaning "no", "as_file", "as_link"
enum class StoreType
{
	DONT_STORE,
	AS_INTERNAL,
	AS_HARD_LINK,
	AS_SOFT_LINK,
	AS_EXTERNAL_LINK
};

// struct for storing the result of the shouldStoreFile function
struct StoreResult
{
	StoreType storeType = StoreType::DONT_STORE;
	std::string datasetPath = "";
	StoreResult(StoreType storeType, std::string datasetPath) : storeType(storeType), datasetPath(datasetPath) {}
	StoreResult(StoreType storeType) : storeType(storeType) {}
	bool operator==(StoreType storeType) { return this->storeType == storeType; }
};

/*
 * Check if a file should be stored in the HDF5 file
 */
StoreResult shouldStore(H5::Group &group, std::string basePath, std::string filepath, std::string datasetName, Opts &opts, bool isDir = false)
{
	bool existing = group.nameExists(datasetName);
	std::string datasetPath = group.getObjName() + "/" + datasetName;
	struct stat result;
	lstat(filepath.c_str(), &result);

	//Check the regexes
	if (!isDir && (!matchesRegex(datasetName, opts["acceptfileregex"], true) || matchesRegex(datasetName, opts["rejectfileregex"], false)))
	{
		return StoreType::DONT_STORE;
	}

	if (isDir && (!matchesRegex(datasetName, opts["acceptdirregex"], true) || matchesRegex(datasetName, opts["rejectdirregex"], false)))
	{
		return StoreType::DONT_STORE;
	}

	// Not symlink and not already existing
	// Store as a file
	if (!S_ISLNK(result.st_mode) && !existing)
	{
		// Check the number of links to the file
		// If it is 1 then store as a file
		if (result.st_nlink == 1)
		{
			return StoreType::AS_INTERNAL;
		}
		else
		{
			// Check in the inode map
			if (inoMap.find(result.st_ino) == inoMap.end())
			{
				inoMap[result.st_ino] = filepath;
				return StoreType::AS_INTERNAL;
			}
			else
			{
				// Link to the existing file
				return StoreResult(StoreType::AS_HARD_LINK, inoMap[result.st_ino]);
			}
		}
	}
	// If the result is a symlink then check if the file that it refers to is below the base path
	if (S_ISLNK(result.st_mode) && !existing)
	{
		char link[PATH_MAX];
		ssize_t len = readlink(filepath.c_str(), link, sizeof(link) - 1);
		// use readlinkat to handle relative symlinks
		if (len != -1)
		{
			link[len] = '\0';
			std::string linkStr = std::string(link);
			// Check if the path is absolute
			// If it isn't then use the base path to make it absolute
			std::filesystem::path linkPath(linkStr);
			std::filesystem::path base(basePath);
			if (!linkPath.is_absolute())
			{
				linkPath = base / linkPath;
			}
			if (is_subpath(base, linkPath))
			{
				return StoreResult(StoreType::AS_SOFT_LINK, linkPath.string());
			}
			else
			{
				// This means that the link is resolved but the file is not below the base path
				// Check the option storeexternalsymlinks
				std::string storeExternalSymlinks = opts.asString("storeexternalsymlinks", "ignore");
				if (storeExternalSymlinks == "ignore")
					return StoreType::DONT_STORE;
				if (storeExternalSymlinks == "file")
					return StoreType::AS_INTERNAL;
				if (storeExternalSymlinks == "singlefile")
				{
					static std::map<std::string, std::string> singleFileMap;
					if (singleFileMap.find(link) == singleFileMap.end())
					{
						singleFileMap[link] = datasetPath;
						return StoreType::AS_INTERNAL;
					}
					else
					{
						return StoreResult(StoreType::AS_SOFT_LINK, singleFileMap[link]);
					}
				}
				if (storeExternalSymlinks == "link")
					return StoreResult(StoreType::AS_EXTERNAL_LINK, std::string(link));
			}
		}
		// Can't track the link, so can't store it
		return StoreType::DONT_STORE;
	}
	// Now know that a file exists and is not a symlink
	// So check the update policy
	std::string updatePolicy = opts.asString("updatepolicy", "never");
	// Never update means don't store
	if (updatePolicy == "never")
	{
		return StoreType::DONT_STORE;
	}
	if (updatePolicy == "always")
	{
		return StoreType::AS_INTERNAL;
	}
	// If the file exists and the update policy is to update on size, then check the size
	if (updatePolicy == "filesize")
	{
		// If the file size is the same, then skip
		hsize_t hs = result.st_size;
		H5::DataSet dataset = group.openDataSet(datasetName);
		if (dataset.getSpace().getSimpleExtentNpoints() == hs)
		{
			dataset.close();
			return StoreType::DONT_STORE;
		}
		dataset.close();
		return StoreType::AS_INTERNAL;
	}
	// If the file exists and the update policy is to update on modification time, then check the modification time
	if (updatePolicy == "filetime")
	{
		// If the file time is the same, then skip
		hsize_t hs = result.st_mtime;
		H5::DataSet dataset = group.openDataSet(datasetName);
		int64_t fileTime;
		dataset.openAttribute("Modified").read(H5::PredType::NATIVE_INT64, &fileTime);
		if (fileTime == hs)
		{
			dataset.close();
			return StoreType::DONT_STORE;
		}
		dataset.close();
		return StoreType::AS_INTERNAL;
	}
	// If the file exists and the update policy is to update on hash, then check the hash
	if ( !isDir && updatePolicy == "hash")
	{
		size_t chunkSize = opts.asInt("chunk", 10 * 1024 * 1024); // Default 10MiB chunk
		// If the hash is the same, then skip
		hsize_t hs = result.st_size;
		H5::DataSet dataset = group.openDataSet(datasetName);
		// Two hex characters per byte plus null terminator
		char hash[PICOHASH_MD5_DIGEST_LENGTH * 2 + 1] = {};
		H5::StrType strtype(H5::PredType::C_S1, PICOHASH_MD5_DIGEST_LENGTH * 2);
		dataset.openAttribute("MD5Hash").read(strtype, hash);
		std::string hashStr(hash);
		picohash_ctx_t ctx;
		picohash_init_md5(&ctx);
		std::ifstream file(filepath, std::ios::binary | std::ios::ate);
		std::streamsize size = file.tellg();
		file.seekg(0, std::ios::beg);
		char *buffer = new char[chunkSize];
		hsize_t offset = 0;
		hsize_t count = chunkSize;
		while (offset < hs)
		{
			if (offset + count > hs)
			{
				count = hs - offset;
			}
			file.read(buffer, count);
			picohash_update(&ctx, buffer, count);
			offset += count;
		}
		file.close();
		unsigned char digest[PICOHASH_MD5_DIGEST_LENGTH];
		picohash_final(&ctx, digest);
		// Convert the digest to a string
		std::string digestStr;
		for (int i = 0; i < PICOHASH_MD5_DIGEST_LENGTH; ++i)
		{
			char hexbuf[3] = {};
			snprintf(hexbuf, 3, "%02x", digest[i]);
			digestStr += hexbuf;
		}
		if (digestStr == hashStr)
		{
			dataset.close();
			delete[] buffer;
			return StoreType::DONT_STORE;
		}
		dataset.close();
		delete[] buffer;
		return StoreType::AS_INTERNAL;
	}

	// Fall through to not storing
	return StoreType::DONT_STORE;
}

/*
 * Check if a directory should be stored in the HDF5 file
 */
StoreResult shouldStoreDirectory(H5::Group &group, std::string basePath, std::string filepath, std::string datasetName, Opts &opts)
{
	//Use lstat to find out if the path is a symlink
	struct stat result;
	lstat(filepath.c_str(), &result);
	//Basically, just check the regexes
	if (matchesRegex(datasetName, opts["acceptdirregex"], true) && !matchesRegex(datasetName, opts["rejectdirregex"], false))
	{
		if (S_ISLNK(result.st_mode))
		{
		char link[PATH_MAX];
		ssize_t len = readlink(filepath.c_str(), link, sizeof(link) - 1);
		// use readlinkat to handle relative symlinks
		if (len != -1)
		{
			link[len] = '\0';
			std::string linkStr = std::string(link);
			// Check if the path is absolute
			// If it isn't then use the base path to make it absolute
			std::filesystem::path linkPath(linkStr);
			std::filesystem::path base(basePath);
			if (!linkPath.is_absolute())
			{
				linkPath = base / linkPath;
			}
			if (is_subpath(base, linkPath))
			{
				return StoreResult(StoreType::AS_SOFT_LINK, linkPath.string());
			}
			else
			{
				// This means that the link is resolved but the file is not below the base path
				// Check the option storeexternalsymlinks
				std::string storeExternalSymlinks = opts.asString("storeexternalsymlinks", "ignore");
				if (storeExternalSymlinks == "ignore")
					return StoreType::DONT_STORE;
				if (storeExternalSymlinks == "file")
					return StoreType::AS_INTERNAL;
				if (storeExternalSymlinks == "singlefile")
				{
					static std::map<std::string, std::string> singleFileMap;
					if (singleFileMap.find(link) == singleFileMap.end())
					{
						singleFileMap[link] = datasetName;
						return StoreType::AS_INTERNAL;
					}
					else
					{
						return StoreResult(StoreType::AS_SOFT_LINK, singleFileMap[link]);
					}
				}
				if (storeExternalSymlinks == "link")
					return StoreResult(StoreType::AS_EXTERNAL_LINK, std::string(link));
			}
		}
		// Can't track the link, so can't store it
		return StoreType::DONT_STORE;
	}
		else
		{
			// If the directory is not a symlink then store it as a directory
			if (result.st_nlink == 1)
			{
				return StoreType::AS_INTERNAL;
			}
			else
			{
				// Check in the inode map
				if (inoMap.find(result.st_ino) == inoMap.end())
				{
					inoMap[result.st_ino] = filepath;
					return StoreType::AS_INTERNAL;
				}
				else
				{
					// Link to the existing file
					return StoreResult(StoreType::AS_HARD_LINK, inoMap[result.st_ino]);
				}
			}
			return StoreType::AS_INTERNAL;
		}
	}
	else
	{
		return StoreType::DONT_STORE;
	}
}

/*
 * Store a file in the HDF5 file
 */
void storeFile(H5::Group &group, std::string filePath, std::string datasetName, Opts &opts)
{
	if (group.nameExists(datasetName))
		group.unlink(datasetName);
	size_t chunkSize = opts.asInt("chunk", 10 * 1024 * 1024); // Default 10MiB chunk
	struct stat result;
	stat(filePath.c_str(), &result);
	hsize_t hs = result.st_size; // File size

	// Open the file and the dataspace
	std::ifstream file(filePath, std::ios::binary);
	H5::DataSpace dataspace(1, &hs);

	// Create the hash object for an MD5 hash
	picohash_ctx_t ctx;
	picohash_init_md5(&ctx);

	// Define chunk size and create property list for chunked dataset
	hsize_t chunk_size[1] = {chunkSize};
	// Limit the chunk size to the file size
	chunk_size[0] = std::min(chunk_size[0], hs);
	// Deal with the special case of an empty file
	if (chunk_size[0] == 0)
	{
		// Hardcoded MD5 hash for empty files
		std::string MD5Hash = "d41d8cd98f00b204e9800998ecf8427e";
		H5::DataSet dataset = group.createDataSet(datasetName, H5::PredType::NATIVE_UINT8, dataspace);
		// Creation time
		dataset.createAttribute("Created", H5::PredType::NATIVE_INT64, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_INT64, &result.st_ctime);
		// Modificiation time
		dataset.createAttribute("Modified", H5::PredType::NATIVE_INT64, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_INT64, &result.st_mtime);
		H5::StrType strtype(H5::PredType::C_S1, MD5Hash.size());
		dataset.createAttribute("MD5Hash", strtype, H5::DataSpace(H5S_SCALAR)).write(strtype, MD5Hash.c_str());
		// Permissions
		dataset.createAttribute("Permissions", H5::PredType::NATIVE_UINT32, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_UINT32, &result.st_mode);
		return;
	}
	H5::DataSet dataset = group.createDataSet(datasetName, H5::PredType::NATIVE_UINT8, dataspace);
	char *buffer = new char[chunk_size[0]];
	hsize_t offset = 0;
	hsize_t count = chunk_size[0];

	while (offset < hs)
	{
		if (offset + count > hs)
		{
			count = hs - offset;
		}

		file.read(buffer, count);
		picohash_update(&ctx, buffer, count);

		// Define hyperslab and write chunk to dataset
		dataspace.selectHyperslab(H5S_SELECT_SET, &count, &offset);
		H5::DataSpace memspace(1, &count);
		dataset.write(buffer, H5::PredType::NATIVE_UINT8, memspace, dataspace);
		offset += count;
	}
	file.close();
	unsigned char digest[PICOHASH_MD5_DIGEST_LENGTH];
	picohash_final(&ctx, digest);
	// Convert the digest to a string
	std::string digestStr;
	for (int i = 0; i < PICOHASH_MD5_DIGEST_LENGTH; ++i)
	{
		char hexbuf[3] = {};
		snprintf(hexbuf, 3, "%02x", digest[i]);
		digestStr += hexbuf;
	}
	H5::StrType strtype(H5::PredType::C_S1, digestStr.size());
	// Store the hash string as an attribute
	dataset.createAttribute("MD5Hash", strtype, H5::DataSpace(H5S_SCALAR)).write(strtype, digestStr.c_str());
	// Creation time
	dataset.createAttribute("Created", H5::PredType::NATIVE_INT64, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_INT64, &result.st_ctime);
	// Modificiation time
	dataset.createAttribute("Modified", H5::PredType::NATIVE_INT64, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_INT64, &result.st_mtime);
	// Permissions
	dataset.createAttribute("Permissions", H5::PredType::NATIVE_UINT32, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_UINT32, &result.st_mode);

	delete[] buffer;
	dataset.close();
}

/*
 * Hard link a file in the HDF5 file
 */
void hardLink(H5::Group &group, std::string sourceDataset, std::string destDataset, Opts &opts)
{
	if (group.nameExists(destDataset))
		group.unlink(destDataset);
	// If the source doesn't exist then defer the link
	if (!group.nameExists(sourceDataset))
	{
		deferredLinks.push_back(std::make_pair(sourceDataset, destDataset));
		return;
	}
	group.link(H5L_TYPE_HARD, sourceDataset.c_str(), destDataset.c_str());
}

/*
 * Soft link a file in the HDF5 file
 */
void softLink(H5::Group &group, std::string sourceDataset, std::string destDataset, Opts &opts)
{
	if (group.nameExists(destDataset))
		group.unlink(destDataset);
	group.link(H5L_TYPE_SOFT, sourceDataset.c_str(), destDataset.c_str());
}


/*
 * External link a file in the HDF5 file
 */
void externalLink(H5::Group &group, std::string sourceFilename, std::string destGroup, Opts &opts)
{
	// Create a group with the name of the destGroup
	// Give it an attribute with the name "ExternalLink" and the value of the sourceFilename
	if (group.nameExists(destGroup))
		group.unlink(destGroup);
	H5::Group externalGroup = group.createGroup(destGroup);
	H5::StrType strtype(H5::PredType::C_S1, sourceFilename.size());
	externalGroup.createAttribute("ExternalLink", strtype, H5::DataSpace(H5S_SCALAR)).write(strtype, sourceFilename.c_str());
	externalGroup.close();
}

/*
 * Link any files that couldn't be hard linked when they were first encountered
 * Usually because the target file didn't exist at that time
 */
void linkDeferredFiles(H5::Group &group)
{
	for (auto &link : deferredLinks)
	{
		if (group.nameExists(link.first))
		{
			group.link(H5L_TYPE_HARD, link.first.c_str(), link.second.c_str());
		}
		else
		{
			std::cerr << "Failed to link " << link.first << " to " << link.second << " as source doesn't exist\n";
		}
	}
	deferredLinks.clear();
}

size_t coalescetoHDF5(int level, const std::string &basePath, const std::string &path, H5::Group &parentGroup, Opts &opts);

// Function to handle a file
size_t handleFile(H5::Group &group, int level, std::string basePath, std::string filePath, Opts &opts)
{
	std::string newName = getLastPathChunk(filePath);
	std::string indent = std::string(level * 2, '-');
	bool existing = group.nameExists(newName);
	auto store = shouldStore(group, basePath, filePath, newName, opts, false);
	if (store == StoreType::DONT_STORE)
	{
		std::cout << indent << "-Skipping dataset " << newName << std::endl;
		return 0;
	}
	if (!existing)
	{
		if (store == StoreType::AS_INTERNAL)
			std::cout << indent << "-Creating dataset " << newName << std::endl;
		else if (store == StoreType::AS_HARD_LINK)
			std::cout << indent << "-Hard linking dataset " << newName << std::endl;
		else if (store == StoreType::AS_SOFT_LINK)
			std::cout << indent << "-Soft linking dataset " << newName << std::endl;
		else if (store == StoreType::AS_EXTERNAL_LINK)
			std::cout << indent << "-Linking dataset " << newName << " to external file " << store.datasetPath << std::endl;
	}
	else
	{
		std::cout << indent << "-Overwriting dataset " << newName << std::endl;
	}
	if (store == StoreType::AS_INTERNAL)
	{
		storeFile(group, filePath, newName, opts);
	}
	else if (store == StoreType::AS_HARD_LINK)
	{
		std::string linkPath = "/" + getLastPathChunk(basePath) + "/" + std::filesystem::relative(store.datasetPath, basePath).string();
		std::string fullname = group.getObjName() + "/" + newName;
		hardLink(group, linkPath, fullname, opts);
	}
	else if (store == StoreType::AS_SOFT_LINK)
	{
		std::string linkPath = "/" + getLastPathChunk(basePath) + "/" + std::filesystem::relative(store.datasetPath, basePath).string();
		std::string fullname = group.getObjName() + "/" + newName;
		softLink(group, linkPath, fullname, opts);
	}
	else if (store == StoreType::AS_EXTERNAL_LINK)
	{
		std::string linkPath = store.datasetPath;
		std::string fullname = group.getObjName() + "/" + newName;
		externalLink(group, linkPath, fullname, opts);
	}
	return 1;
}

// Function to handle a directory
size_t handleDirectory(H5::Group &parentGroup, int level, std::string basePath, std::string dirPath, Opts &opts)
{
	std::string indent = std::string(level * 2, '-');
	std::string newName = getLastPathChunk(dirPath);
	bool existingGroup = parentGroup.nameExists(newName);
	H5::Group group;

	auto storeType = shouldStore(parentGroup, basePath, dirPath, newName, opts,true);

	if (storeType == StoreType::DONT_STORE)
	{
		std::cout << indent << "Skipping directory " << newName << std::endl;
		return 0;
	}

	if (storeType == StoreType::AS_SOFT_LINK)
	{
		std::string linkPath = "/" + getLastPathChunk(basePath) + "/" + std::filesystem::relative(storeType.datasetPath, basePath).string();
		std::string fullname = parentGroup.getObjName() + "/" + newName;
		softLink(parentGroup, linkPath, fullname, opts);
		std::cout << indent << "Soft linking directory " << newName << " to " << linkPath << "\n";
		return 1;
	}

	if (existingGroup)
	{
		std::cout << indent << "Opening existing group " << newName << "\n";
		existingGroup = true;
		group = parentGroup.openGroup(newName);
	}
	else
	{
		std::cout << indent << "Creating group " << newName << "\n";
		// Get information about the directory
		struct stat result;
		stat(dirPath.c_str(), &result);
		// Create the group
		group = parentGroup.createGroup(newName);
		// Creation time
		group.createAttribute("Created", H5::PredType::NATIVE_INT64, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_INT64, &result.st_ctime);
		// Modificiation time
		group.createAttribute("Modified", H5::PredType::NATIVE_INT64, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_INT64, &result.st_mtime);
		//Permissions
		group.createAttribute("Permissions", H5::PredType::NATIVE_UINT32, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_UINT32, &result.st_mode);
	}
	size_t itemCount = 0;
	for (const auto &entry : std::filesystem::directory_iterator(dirPath))
	{
		itemCount += coalescetoHDF5(level + 1, basePath, entry.path().string(), group, opts);
	}

	if (itemCount==0 && !existingGroup && !opts.asBool("allowemptydirs", false))
	{
		std::cout << indent << "Removing group " << newName << " as empty\n";
		parentGroup.unlink(newName);
	}
	group.close();
	return itemCount;
}

size_t coalescetoHDF5(int level, const std::string &basePath, const std::string &path, H5::Group &parentGroup, Opts &opts)
{
	size_t itemCount = 0;
	//Check the type of path
	struct stat result;
	stat(path.c_str(), &result);
	if (S_ISDIR(result.st_mode))
	{
		itemCount+=(handleDirectory(parentGroup, level, basePath, path, opts));
	} else if (S_ISREG(result.st_mode))
	{
		itemCount+=(handleFile(parentGroup, level, basePath, path, opts));
	}
	else if (S_ISLNK(result.st_mode))
	{
		// If it is a symlink then store it as a file
		itemCount+=(handleFile(parentGroup, level, basePath, path, opts));
	} else {
	}
	return itemCount;
}

/**
 * Print the usage information
 */
void printUsage()
{
	std::cout << "\nUsage\n";
	std::cout << "-----\n";
	std::cout << "toHDF5 {directory} [--acceptfile={} --acceptfileregex={} --rejectfile={} --rejectfileregex={} --allowdir={} --allowdirregex={} --rejectdir={} --rejectdirregex={} --chunk=N --output={}]\n\n";
	std::cout << "directory - The directory to recursively convert to an HDF5 file. Multiple directories can be specified, but if they are then an output filename MUST be specified with --output\n";
	std::cout << "acceptfile - A filename or wildcard that says what files to add to the HDF5 file\n";
	std::cout << "acceptfileregex - A grep-like regex for what files to add to the HDF5 file\n";
	std::cout << "rejectfile - A filename or wildcard that says what files should be excluded from the HDF5 file\n";
	std::cout << "rejectfileregex - A grep-like regex for what files to add to the HDF5 file\n";
	std::cout << "You can have as many allow and reject keys on the command line as wanted. Matches are done on a key by key basis\n";
	std::cout << "When working out if a file will be included, it must be in an \"allow\" expression if there are any \"allow\" expressions, and must not be in any \"reject\" expressions if there are any \"reject\" expressions\n";
	std::cout << "allowdir - A directory name or wildcard that says what directories to include in the HDF5 file\n";
	std::cout << "allowdirregex - A grep-like regex for what directories to include in the HDF5 file\n";
	std::cout << "rejectdir - A directory name or wildcard that says what directories to exclude from the HDF5 file\n";
	std::cout << "rejectdirregex - A grep-like regex for what directories to exclude from the HDF5 file\n";
	std::cout << "chunk - A size in bytes for the size of chunks to use when writing files into the HDF5 file. Default 10MiB\n";
	std::cout << "output - The output filename for the generated HDF5 file. By default is the name of the directory being coalesced into an HDF5 file with an .h5 extension\n";
	std::cout << "updatepolicy - The policy for updating files in the HDF5 file. Can be one of never, always, filesize, filetime or hash. Default is never\n never - Never update the file in the HDF5 file\n always - Always update the file in the HDF5 file\n filesize - Update the file in the HDF5 file if the file size has changed\n filetime - Update the file in the HDF5 file if the file modification time has changed\n hash - Update the file in the HDF5 file if the file hash has changed (MD5 hash). Note that this option may be slow as files must be read to calculate the hash\n";
	std::cout << "newroots - If you are extending an existing HDF5 file with new root directories, then this must be specified\n";
	std::cout << "storeexternalsymlinks - If a symlink points to a file outside the base directory, then this specifies what to do. Can be one of ignore, file, singlefile or link. Default is ignore.\n ignore - Ignore the symlink.\n file - Store the symlink as a file.\n singlefile - Store the symlink as a file, but only store one copy of the file. Other symlinks to the same file will be soft linked to the stored file.\n link - Keep the symlink as a symlink and don't store the file in the HDF5 file. This file will not work on other systems unless the symlink is resolved.\n";
	std::cout << "allowemptydirs - If a directory is empty, then it will be removed from the HDF5 file. This option stops that behaviour\n";
}

int main(int argc, char **argv)
{
	std::string version = VERSION;
	std::string vs = VERSIONSTRING;
	std::cout << "\n"
			  << vs << "\n";
	std::cout << std::string(vs.size(), '=') << "\n\n";

	if (argc == 1)
	{
		printUsage();
		return -1;
	}

	Opts params;
	params.addKey("path");
	params.addKey("help");
	params.addKey("acceptfile", true);
	params.addKey("acceptfileregex", true);
	params.addKey("acceptdir", true);
	params.addKey("acceptdirregex", true);
	params.addKey("rejectfile", true);
	params.addKey("rejectfileregex", true);
	params.addKey("rejectdir", true);
	params.addKey("rejectdirregex", true);
	params.addKey("chunk");
	params.addKey("output");
	params.addKey("updatepolicy");
	params.addKey("newroots");
	params.addKey("storeexternalsymlinks");
	params.addKey("allowemptydirs");
	params.parse(argc, argv);
	if (params.present("help"))
	{
		printUsage();
		return 0;
	}

	if (params.present("updatepolicy"))
	{
		bool updatepolicyok = false;
		if (params.asString("updatepolicy") == "never")
		{
			updatepolicyok = true;
			std::cout << "Update policy set to never\n";
		}
		if (params.asString("updatepolicy") == "filesize")
		{
			updatepolicyok = true;
			std::cout << "Update policy set to filesize\n";
		}
		if (params.asString("updatepolicy") == "filetime")
		{
			updatepolicyok = true;
			std::cout << "Update policy set to filetime\n";
		}
		if (params.asString("updatepolicy") == "hash")
		{
			updatepolicyok = true;
			std::cout << "Update policy set to hash\n";
		}
		if (params.asString("updatepolicy") == "always")
		{
			updatepolicyok = true;
			std::cout << "Update policy set to always\n";
		}
		if (!updatepolicyok)
		{
			std::cerr << "Invalid update policy. Must be one of never, always, filesize, filetime or hash\n";
			exit(-1);
		}
	}

	if (!params.present("path"))
	{
		std::cerr << "Must specify a directory to coalesce\n";
		exit(-1);
	}

	for (auto &file : params["acceptfile"])
	{
		params["acceptfileregex"].push_back(globToRegex(file));
	}

	for (auto &dir : params["allowdir"])
	{
		params["allowdirregex"].push_back(globToRegex(dir));
	}

	for (auto &file : params["rejectfile"])
	{
		params["rejectfileregex"].push_back(globToRegex(file));
	}

	for (auto &dir : params["rejectdir"])
	{
		params["rejectdirregex"].push_back(globToRegex(dir));
	}

	try
	{
		// Make the filename the same as the directory name
		std::string filename;
		if (params["path"].size() > 1 && !params.present("output"))
		{
			std::cerr << "If coalescing multiple directorys an output file must be specified. Use --help to see usage\n";
			return -1;
		}

		for (auto &path : params["path"])
		{
			std::filesystem::path p(path);
			std::filesystem::path ap = std::filesystem::weakly_canonical(p);
			path = ap.string();
			std::cout << "Path = " << path << "\n";
		}

		// If this ever runs then it will produce an odd output for multiple directories to coalesce, but that shouldn't
		// be a problem
		filename = getLastPathChunk(params["path"][0]);
		filename += ".h5";
		filename = params.asString("output", filename);
		H5::H5File file;
		H5::Group rootGroup;
		H5::Exception::dontPrint();
		try
		{
			file = H5::H5File(filename, H5F_ACC_RDWR);
			rootGroup = file.openGroup("/");
			for (auto &path : params["path"])
			{
				if (!file.nameExists(getLastPathChunk(path)) && !params.asBool("newroots", false))
				{
					std::cerr << "Extending a file to include new root groups is only possible with the --newroots parameter\n";
					file.close();
					return -1;
				}
				std::cout << "Appending to file " << filename << "\n";
			}
		}
		catch (const H5::FileIException &)
		{
			file = H5::H5File(filename, H5F_ACC_TRUNC);
			rootGroup = file.openGroup("/");
			// Create an attribute to store that this is an H5VFS file
			H5::StrType strtype(H5::PredType::C_S1, version.size());
			rootGroup.createAttribute("H5VFS", strtype, H5::DataSpace(H5S_SCALAR)).write(strtype, version.c_str());
			// Create another attribute storing the current date and time as an int64
			int64_t now = time(NULL);
			rootGroup.createAttribute("Created", H5::PredType::NATIVE_INT64, H5::DataSpace(H5S_SCALAR)).write(H5::PredType::NATIVE_INT64, &now);

			std::cout << "Creating new file " << filename << "\n";
		}
		H5::Exception::printErrorStack();

		bool defaultRoot = false;

		size_t itemCount = 0;
		for (auto &path : params["path"])
		{
			itemCount = coalescetoHDF5(1, path, path, rootGroup, params);
			linkDeferredFiles(rootGroup);
		}
		file.close();
		if (itemCount > 0)
		{
			std::cout << "Coalescence completed successfully\n";
		}
		else
		{
			std::cout << "Coalescence completed successfully, but no files added\n";
		}
	}
	catch (const H5::Exception &e)
	{
		std::cerr << "Error: " << e.getDetailMsg() << std::endl;
		return 1;
	}

	return 0;
}
