#include "httplib.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <unordered_map>
#include <string>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <mutex>

struct Medicine {
    int id;
    std::string genericName;
    std::vector<std::string> brands;
    std::string category;
    bool rxRequired;
};

struct Pharmacy {
    std::string name;
    std::string area;
    double lat;
    double lon;
    std::string pin;
    std::string ppbLicense;
    bool approved;
    std::set<int> stockIds;
};

struct SearchResult {
    Pharmacy pharmacy;
    double distanceKm;
    std::vector<int> availableIds;
    std::vector<int> missingIds;
    double matchRatio;
};

std::string toLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    if (s.size() >= 3 && (unsigned char)s[0]==0xEF && (unsigned char)s[1]==0xBB && (unsigned char)s[2]==0xBF) return trim(s.substr(3)); size_t start = s.find_first_not_of(" \t\r\n");
    size_t end = s.find_last_not_of(" \t\r\n");
    return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

double haversineKm(double lat1, double lon1, double lat2, double lon2) {
    constexpr double R = 6371.0;
    constexpr double PI = 3.14159265358979323846;
    double dLat = (lat2 - lat1) * PI / 180.0;
    double dLon = (lon2 - lon1) * PI / 180.0;
    lat1 = lat1 * PI / 180.0;
    lat2 = lat2 * PI / 180.0;

    double a = std::sin(dLat / 2) * std::sin(dLat / 2) +
               std::cos(lat1) * std::cos(lat2) *
               std::sin(dLon / 2) * std::sin(dLon / 2);
    return R * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

int loadCatalog(const std::string& filename,
                std::unordered_map<int, Medicine>& idToMed,
                std::unordered_map<std::string, int>& nameToId) {
    std::ifstream file(filename);
    std::string line;
    std::getline(file, line);
    int maxId = 100;

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string idStr, generic, brandsStr, category, rxStr;

        std::getline(ss, idStr, ',');
        std::getline(ss, generic, ',');
        std::getline(ss, brandsStr, ',');
        std::getline(ss, category, ',');
        std::getline(ss, rxStr, ',');

        int id = 0; try { id = std::stoi(trim(idStr)); } catch (...) { continue; }
        if (id > maxId) maxId = id;
        std::string cleanGeneric = toLower(trim(generic));
        bool rx = (trim(rxStr) == "1");

        Medicine med{id, cleanGeneric, {}, trim(category), rx};
        nameToId[cleanGeneric] = id;

        std::stringstream bStream(brandsStr);
        std::string b;
        while (std::getline(bStream, b, '|')) {
            std::string cleanBrand = toLower(trim(b));
            if (!cleanBrand.empty()) {
                med.brands.push_back(cleanBrand);
                nameToId[cleanBrand] = id;
            }
        }
        idToMed[id] = med;
    }
    return maxId;
}

std::vector<Pharmacy> loadPharmacies(const std::string& filename) {
    std::vector<Pharmacy> list;
    std::ifstream file(filename);
    std::string line;
    std::getline(file, line);

    while (std::getline(file, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string name, area, latStr, lonStr, pinStr, ppbStr, appStr, stockStr;

        std::getline(ss, name, ',');
        std::getline(ss, area, ',');
        std::getline(ss, latStr, ',');
        std::getline(ss, lonStr, ',');
        std::getline(ss, pinStr, ',');
        std::getline(ss, ppbStr, ',');
        std::getline(ss, appStr, ',');
        std::getline(ss, stockStr, ',');

        try { Pharmacy p{
            trim(name), trim(area),
            std::stod(latStr), std::stod(lonStr),
            trim(pinStr), trim(ppbStr),
            trim(appStr) == "1", {}
        };
        std::stringstream stockStream(stockStr);
        std::string idToken;
        while (std::getline(stockStream, idToken, '|')) {
            std::string cleanId = trim(idToken);
            if (!cleanId.empty()) p.stockIds.insert(std::stoi(cleanId));
        }
        list.push_back(p); } catch (...) { continue; }
    }
    return list;
}

void savePharmacies(const std::string& filename, const std::vector<Pharmacy>& pharmacies) {
    std::ofstream out(filename);
    out << "name,area,lat,lon,pin,ppb_license,approved,stock_ids\n";
    for (const auto& p : pharmacies) {
        out << p.name << "," << p.area << "," << p.lat << "," << p.lon << ","
            << p.pin << "," << p.ppbLicense << "," << (p.approved ? "1" : "0") << ",";
        size_t count = 0;
        for (int id : p.stockIds) {
            out << id << (++count < p.stockIds.size() ? "|" : "");
        }
        out << "\n";
    }
}

int main() {
    std::unordered_map<int, Medicine> idToMed;
    std::unordered_map<std::string, int> nameToId;
    int maxMedId = loadCatalog("medicines_catalog.csv", idToMed, nameToId);

    std::vector<Pharmacy> pharmacies = loadPharmacies("pharmacies.csv");
    std::mutex dataMutex;
    const std::string MASTER_KEY = "MEDROUTE2026";

    std::cout << "Loaded " << idToMed.size() << " medicines and " << pharmacies.size() << " pharmacies.\n";

    httplib::Server svr;

    svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream html("index.html");
        std::stringstream buffer;
        buffer << html.rdbuf();
        res.set_content(buffer.str(), "text/html");
    });

    svr.Get("/manifest.json", [](const httplib::Request&, httplib::Response& res) { std::ifstream f("manifest.json"); std::stringstream b; b << f.rdbuf(); res.set_content(b.str(), "application/json"); });

    svr.Get("/icon.svg", [](const httplib::Request&, httplib::Response& res) { std::ifstream f("icon.svg"); std::stringstream b; b << f.rdbuf(); res.set_content(b.str(), "image/svg+xml"); });

    svr.Get("/admin", [](const httplib::Request&, httplib::Response& res) {
        std::ifstream html("admin.html");
        std::stringstream buffer;
        buffer << html.rdbuf();
        res.set_content(buffer.str(), "text/html");
    });

    svr.Get("/api/catalog", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dataMutex);
        std::ostringstream json;
        json << "[";
        size_t count = 0;
        for (const auto& pair : idToMed) {
            const auto& m = pair.second;
            json << "{\"id\":" << m.id
                 << ",\"generic\":\"" << jsonEscape(m.genericName) << "\""
                 << ",\"category\":\"" << jsonEscape(m.category) << "\""
                 << ",\"rx\":" << (m.rxRequired ? "true" : "false")
                 << ",\"brands\":[";
            for (size_t i = 0; i < m.brands.size(); ++i)
                json << "\"" << jsonEscape(m.brands[i]) << "\"" << (i + 1 < m.brands.size() ? "," : "");
            json << "]}";
            if (++count < idToMed.size()) json << ",";
        }
        json << "]";
        res.set_content(json.str(), "application/json");
    });

    svr.Get("/api/pharmacies", [&](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(dataMutex);
        std::ostringstream json;
        json << "[";
        for (size_t i = 0; i < pharmacies.size(); ++i) {
            const auto& p = pharmacies[i];
            json << "{\"index\":" << i
                 << ",\"name\":\"" << jsonEscape(p.name) << "\""
                 << ",\"area\":\"" << jsonEscape(p.area) << "\""
                 << ",\"lat\":" << p.lat
                 << ",\"lon\":" << p.lon
                 << ",\"ppb\":\"" << jsonEscape(p.ppbLicense) << "\""
                 << ",\"approved\":" << (p.approved ? "true" : "false")
                 << ",\"stockIds\":[";
            size_t c = 0;
            for (int id : p.stockIds) json << id << (++c < p.stockIds.size() ? "," : "");
            json << "]}";
            if (i + 1 < pharmacies.size()) json << ",";
        }
        json << "]";
        res.set_content(json.str(), "application/json");
    });

    // Self-register a new pharmacy branch (starts as Pending until Master Admin approves)
    svr.Post("/api/register_pharmacy", [&](const httplib::Request& req, httplib::Response& res) {
        std::string name = req.has_param("name") ? trim(req.get_param_value("name")) : "";
        std::string area = req.has_param("area") ? trim(req.get_param_value("area")) : "";
        std::string ppb = req.has_param("ppb") ? trim(req.get_param_value("ppb")) : "";
        std::string latStr = req.has_param("lat") ? trim(req.get_param_value("lat")) : "";
        std::string lonStr = req.has_param("lon") ? trim(req.get_param_value("lon")) : "";
        std::string pin = req.has_param("pin") ? trim(req.get_param_value("pin")) : "";

        std::replace(name.begin(), name.end(), ',', '-');
        std::replace(area.begin(), area.end(), ',', '-');
        std::replace(ppb.begin(), ppb.end(), ',', '-');

        if (name.empty() || area.empty() || ppb.empty() || latStr.empty() || lonStr.empty() || pin.length() < 4) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"msg\":\"Fill all fields (PIN must be at least 4 digits).\"}", "application/json");
            return;
        }

        try {
            double lat = std::stod(latStr);
            double lon = std::stod(lonStr);
            std::lock_guard<std::mutex> lock(dataMutex);
            pharmacies.push_back(Pharmacy{name, area, lat, lon, pin, ppb, false, {101, 102, 111}});
            savePharmacies("pharmacies.csv", pharmacies);
            res.set_content("{\"ok\":true,\"msg\":\"Submitted! Your branch is Pending verification before going live.\"}", "application/json");
        } catch (...) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"msg\":\"Invalid GPS coordinates.\"}", "application/json");
        }
    });

    // Approve or delete a pending branch using Master Key
    svr.Post("/api/verify_pharmacy", [&](const httplib::Request& req, httplib::Response& res) {
        std::string master = req.has_param("master") ? trim(req.get_param_value("master")) : "";
        if (master != MASTER_KEY) {
            res.status = 403;
            res.set_content("{\"ok\":false,\"msg\":\"Invalid Master Admin Code.\"}", "application/json");
            return;
        }
        size_t idx = std::stoul(req.get_param_value("index"));
        std::string action = req.get_param_value("action");

        std::lock_guard<std::mutex> lock(dataMutex);
        if (idx >= pharmacies.size()) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"msg\":\"Invalid branch index.\"}", "application/json");
            return;
        }
        if (action == "approve") {
            pharmacies[idx].approved = true;
            savePharmacies("pharmacies.csv", pharmacies);
            res.set_content("{\"ok\":true,\"msg\":\"Branch approved and now live on map searches!\"}", "application/json");
        } else {
            pharmacies.erase(pharmacies.begin() + idx);
            savePharmacies("pharmacies.csv", pharmacies);
            res.set_content("{\"ok\":true,\"msg\":\"Branch removed.\"}", "application/json");
        }
    });

    svr.Post("/api/add_medicine", [&](const httplib::Request& req, httplib::Response& res) {
        size_t idx = std::stoul(req.get_param_value("index"));
        std::string pin = trim(req.get_param_value("pin"));
        std::string generic = toLower(trim(req.get_param_value("generic")));
        std::string brandsRaw = req.has_param("brands") ? trim(req.get_param_value("brands")) : "";
        std::string category = trim(req.get_param_value("category"));
        bool rx = (req.has_param("rx") && trim(req.get_param_value("rx")) == "1");

        std::replace(generic.begin(), generic.end(), ',', ' ');
        std::replace(category.begin(), category.end(), ',', ' ');
        std::replace(brandsRaw.begin(), brandsRaw.end(), ',', '|');

        std::lock_guard<std::mutex> lock(dataMutex);
        if (idx >= pharmacies.size() || pharmacies[idx].pin != pin) {
            res.status = 403;
            res.set_content("{\"ok\":false,\"msg\":\"Invalid PIN for this pharmacy\"}", "application/json");
            return;
        }
        if (generic.empty() || category.empty() || nameToId.count(generic)) {
            res.status = 400;
            res.set_content("{\"ok\":false,\"msg\":\"Medicine is empty or already exists in catalog.\"}", "application/json");
            return;
        }

        int newId = ++maxMedId;
        Medicine med{newId, generic, {}, category, rx};
        nameToId[generic] = newId;

        std::vector<std::string> cleanBrands;
        std::stringstream bStream(brandsRaw);
        std::string b;
        while (std::getline(bStream, b, '|')) {
            std::string cb = toLower(trim(b));
            if (!cb.empty()) {
                med.brands.push_back(cb);
                cleanBrands.push_back(cb);
                nameToId[cb] = newId;
            }
        }
        idToMed[newId] = med;

        std::ofstream catOut("medicines_catalog.csv", std::ios::app);
        catOut << newId << "," << generic << ",";
        for (size_t i = 0; i < cleanBrands.size(); ++i)
            catOut << cleanBrands[i] << (i + 1 < cleanBrands.size() ? "|" : "");
        catOut << "," << category << "," << (rx ? "1" : "0") << "\n";

        pharmacies[idx].stockIds.insert(newId);
        savePharmacies("pharmacies.csv", pharmacies);
        res.set_content("{\"ok\":true,\"msg\":\"Added new medicine and ticked in your stock!\"}", "application/json");
    });

    svr.Post("/api/update_stock", [&](const httplib::Request& req, httplib::Response& res) {
        size_t idx = std::stoul(req.get_param_value("index"));
        std::string pin = trim(req.get_param_value("pin"));
        std::string idsParam = req.has_param("ids") ? req.get_param_value("ids") : "";

        std::lock_guard<std::mutex> lock(dataMutex);
        if (idx >= pharmacies.size() || pharmacies[idx].pin != pin) {
            res.status = 403;
            res.set_content("{\"ok\":false,\"msg\":\"Invalid PIN for this pharmacy\"}", "application/json");
            return;
        }

        std::set<int> updatedStock;
        std::stringstream ss(idsParam);
        std::string token;
        while (std::getline(ss, token, ',')) {
            std::string clean = trim(token);
            if (!clean.empty()) updatedStock.insert(std::stoi(clean));
        }

        pharmacies[idx].stockIds = updatedStock;
        savePharmacies("pharmacies.csv", pharmacies);
        res.set_content("{\"ok\":true,\"msg\":\"Inventory updated and saved!\"}", "application/json");
    });

    // Only approved pharmacies appear in traveler search results
    svr.Get("/api/search", [&](const httplib::Request& req, httplib::Response& res) {
        double userLat = req.has_param("lat") ? std::stod(req.get_param_value("lat")) : -1.2648;
        double userLon = req.has_param("lon") ? std::stod(req.get_param_value("lon")) : 36.8047;

        std::string idsParam = req.has_param("ids") ? req.get_param_value("ids") : "";

        std::set<int> userNeeds;
        std::stringstream ss(idsParam);
        std::string token;
        while (std::getline(ss, token, ',')) {
            std::string clean = toLower(trim(token));
            if (clean.empty()) continue;
            if (std::all_of(clean.begin(), clean.end(), ::isdigit)) userNeeds.insert(std::stoi(clean));
            else if (nameToId.count(clean)) userNeeds.insert(nameToId[clean]);
        }

        if (userNeeds.empty()) {
            res.set_content("[]", "application/json");
            return;
        }

        std::vector<SearchResult> results;
        {
            std::lock_guard<std::mutex> lock(dataMutex);
            for (const auto& p : pharmacies) {
                if (!p.approved) continue;
                std::vector<int> available, missing;
                std::set_intersection(userNeeds.begin(), userNeeds.end(), p.stockIds.begin(), p.stockIds.end(), std::back_inserter(available));
                std::set_difference(userNeeds.begin(), userNeeds.end(), p.stockIds.begin(), p.stockIds.end(), std::back_inserter(missing));

                if (!available.empty()) {
                    double dist = haversineKm(userLat, userLon, p.lat, p.lon);
                    double ratio = static_cast<double>(available.size()) / userNeeds.size();
                    results.push_back({p, dist, available, missing, ratio});
                }
            }
        }

        std::sort(results.begin(), results.end(), [](const SearchResult& a, const SearchResult& b) {
            if (a.matchRatio != b.matchRatio) return a.matchRatio > b.matchRatio;
            return a.distanceKm < b.distanceKm;
        });

        auto formatMed = [&](int id) {
            if (!idToMed.count(id)) return std::to_string(id);
            const auto& m = idToMed[id];
            return m.genericName + (m.rxRequired ? " [Rx]" : "");
        };

        std::ostringstream json;
        json << "[";
        for (size_t i = 0; i < results.size(); ++i) {
            const auto& r = results[i];
            json << "{"
                 << "\"name\":\"" << jsonEscape(r.pharmacy.name) << "\","
                 << "\"area\":\"" << jsonEscape(r.pharmacy.area) << "\","
                 << "\"lat\":" << r.pharmacy.lat << ","
                 << "\"lon\":" << r.pharmacy.lon << ","
                 << "\"distanceKm\":" << std::round(r.distanceKm * 10.0) / 10.0 << ","
                 << "\"matchPercent\":" << static_cast<int>(r.matchRatio * 100) << ","
                 << "\"available\":[";
            for (size_t j = 0; j < r.availableIds.size(); ++j)
                json << "\"" << jsonEscape(formatMed(r.availableIds[j])) << "\"" << (j + 1 < r.availableIds.size() ? "," : "");
            json << "],\"missing\":[";
            for (size_t j = 0; j < r.missingIds.size(); ++j)
                json << "\"" << jsonEscape(formatMed(r.missingIds[j])) << "\"" << (j + 1 < r.missingIds.size() ? "," : "");
            json << "]}";
            if (i + 1 < results.size()) json << ",";
        }
        json << "]";

        res.set_content(json.str(), "application/json");
    });

    const char* portEnv = std::getenv("PORT");
    int port = portEnv ? std::stoi(portEnv) : 8080;
    std::cout << "MedRoute running at http://localhost:" << port << "\n";
    svr.listen("0.0.0.0", port);
    return 0;
}




