#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
	((std::string*)userp)->append((char*)contents, size * nmemb);
	return size * nmemb;
}

// Renamed to avoid conflict with Windows API WriteFile function
size_t WriteFileCallback(void* ptr, size_t size, size_t nmemb, void* userdata) {
	std::ofstream* stream = static_cast<std::ofstream*>(userdata);
	stream->write(static_cast<char*>(ptr), size * nmemb);
	return size * nmemb;
}

bool fetchPage(const std::string& orderId, int pageNo, json& outJson) {
	CURL* curl = curl_easy_init();
	if (!curl) return false;

	std::string buffer;
	std::string url = "https://www.yipai360.com/applet/v2/photo/select-page";
	std::string postFields = "orderId=" + orderId + "&tagId=&sortType=desc&sortField=createDateTime&pageNo=" + std::to_string(pageNo);

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
		return false;
	}
	try {
		outJson = json::parse(buffer);
	}
	catch (json::parse_error& e) {
		std::cerr << "JSON parse error: " << e.what() << std::endl;
		return false;
	}
	return true;
}

bool downloadPhoto(const std::string& etag, const std::string& fname) {
	fs::create_directories("downloads");
	fs::path filePath = fs::path("downloads") / fname;
	std::ofstream file(filePath, std::ios::binary);
	if (!file) {
		std::cerr << "Cannot open file for writing: " << filePath << std::endl;
		return false;
	}

	CURL* curl = curl_easy_init();
	if (!curl) return false;
	std::string url = "https://c360-o2o.c360dn.com/" + etag;

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &file);

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK) {
		std::cerr << "Failed to download " << fname << ": "
			<< curl_easy_strerror(res) << std::endl;
		return false;
	}
	return true;
}

int main() {
	std::ifstream cfg("config.json");
	if (!cfg) {
		std::cerr << "Cannot open config.json" << std::endl;
		return 1;
	}
	json config;
	try {
		cfg >> config;
	}
	catch (json::parse_error& e) {
		std::cerr << "Failed to parse config.json: " << e.what() << std::endl;
		return 1;
	}

	std::string orderId = config.value("orderId", "");
	if (orderId.empty()) {
		std::cerr << "orderId missing in config.json" << std::endl;
		return 1;
	}

	std::vector<std::string> names;
	if (config.contains("fname") && config["fname"].is_array()) {
		for (auto& n : config["fname"]) {
			names.push_back(n.get<std::string>());
		}
	}

	std::unordered_map<std::string, std::string> nameToEtag;

	// Overwrite photo_data.json on each run to keep the file size small
	std::ofstream out("photo_data.json", std::ios::trunc);
	if (!out) {
		std::cerr << "Cannot open photo_data.json" << std::endl;
		return 1;
	}

	int pageNo = 1;
	while (true) {
		json page;
		if (!fetchPage(orderId, pageNo, page)) {
			std::cerr << "Request for page " << pageNo << " failed" << std::endl;
			break;
		}

		auto& data = page["data"];

		std::cout << "Writing photo album page " << pageNo << " data to file.\n";

		for (auto& item : data["result"]) {
			out << item.dump() << '\n';
			std::string fname = item.value("fname", "");
			std::string etag = item.value("etag", "");
			if (!fname.empty()) {
				nameToEtag[fname] = etag;
			}
		}

		int respPageNo = data.value("pageNo", pageNo);
		int totalPage = data.value("totalPage", respPageNo);
		if (respPageNo >= totalPage) {
			break;
		}
		pageNo = respPageNo + 1;
	}

	for (const auto& fname : names) {
		auto it = nameToEtag.find(fname);
		if (it != nameToEtag.end()) {
			std::cout << fname << ": " << it->second << std::endl;
			if (downloadPhoto(it->second, fname)) {
				std::cout << "saved to downloads/" << fname << std::endl;
			}
			else {
				std::cout << "failed to download " << fname << std::endl;
			}
		}
		else {
			std::cout << fname << ": not found" << std::endl;
		}
	}

	return 0;
}
