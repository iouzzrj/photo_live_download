#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <filesystem>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <cctype>

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

std::string sanitizeFilename(const std::string& input) {
        std::string result;
        for (unsigned char c : input) {
                switch (c) {
                case '/': case '\\': case '?': case '%': case '*': case ':':
                case '|': case '"': case '<': case '>': case '\n': case '\r':
                        result += '_';
                        break;
                default:
                        result += c;
                        break;
                }
        }
        auto start = result.find_first_not_of(' ');
        auto end = result.find_last_not_of(' ');
        if (start == std::string::npos) return "";
        return result.substr(start, end - start + 1);
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

bool fetchDetail(const std::string& orderId, json& outJson) {
        CURL* curl = curl_easy_init();
        if (!curl) return false;

        std::string buffer;
        std::string url = "https://www.yipai360.com/applet/v2/order/detail";
        std::string postFields = "orderId=" + orderId;

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

bool downloadPhoto(const std::string& etag, const std::string& fname, const fs::path& dir) {
        fs::path filePath = dir / fname;
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

        json detail;
        if (!fetchDetail(orderId, detail)) {
                std::cerr << "Failed to fetch order detail" << std::endl;
                return 1;
        }
        std::string title = detail["data"].value("title", "album");
        std::string dirName = sanitizeFilename(title);
        if (dirName.empty()) dirName = "album";
        fs::path baseDir = "downloads";
        fs::create_directories(baseDir);
        // Use u8path to safely handle UTF-8 titles (e.g., Chinese characters)
        fs::path downloadDir = baseDir / fs::u8path(dirName);
        fs::create_directories(downloadDir);

        json history = json::array();
        std::set<std::pair<std::string, std::string>> historySet;
        {
                std::ifstream hf("history.json");
                if (hf) {
                        try {
                                hf >> history;
                                for (auto& entry : history) {
                                        std::string oid = entry.value("orderid", "");
                                        if (entry.contains("files") && entry["files"].is_array()) {
                                                for (auto& f : entry["files"]) {
                                                        historySet.emplace(oid, f.value("etag", ""));
                                                }
                                        }
                                }
                        }
                        catch (json::parse_error&) {
                                history = json::array();
                        }
                }
        }

        std::unordered_set<std::string> names;
        if (config.contains("fname") && config["fname"].is_array()) {
                for (auto& n : config["fname"]) {
                        names.insert(n.get<std::string>());
                }
        }

        int pageNo = 1;
        while (!names.empty()) {
                json page;
                if (!fetchPage(orderId, pageNo, page)) {
                        std::cerr << "Request for page " << pageNo << " failed" << std::endl;
                        break;
                }

                auto& data = page["data"];

                for (auto& item : data["result"]) {
                        std::string fname = item.value("fname", "");
                        std::string etag = item.value("etag", "");
                        if (!fname.empty() && names.count(fname)) {
                                fs::path filePath = downloadDir / fname;
                                auto key = std::make_pair(orderId, etag);
                                if (fs::exists(filePath)) {
                                        std::cout << fname << ": already downloaded" << std::endl;
                                }
                                else {
                                        if (downloadPhoto(etag, fname, downloadDir)) {
                                                if (historySet.insert(key).second) {
                                                        auto it = std::find_if(history.begin(), history.end(), [&](const json& j) {
                                                                return j.value("orderid", "") == orderId;
                                                        });
                                                        if (it == history.end()) {
                                                                history.push_back({{"orderid", orderId}, {"title", title}, {"files", json::array()}});
                                                                it = std::prev(history.end());
                                                        }
                                                        (*it)["files"].push_back({{"etag", etag}, {"fname", fname}});
                                                }
                                                std::cout << "saved to " << filePath << std::endl;
                                        }
                                        else {
                                                std::cout << "failed to download " << fname << std::endl;
                                        }
                                }
                                names.erase(fname);
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
                std::cout << fname << ": not found" << std::endl;
        }

        // Persist updated history so future runs skip already downloaded files
        std::ofstream histOut("history.json");
        histOut << history.dump(4);

        return 0;
}
