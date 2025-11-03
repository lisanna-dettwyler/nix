#include "nix/util/base-n.hh"
#include "nix/store/machines.hh"
#include "nix/store/globals.hh"
#include "nix/store/store-open.hh"
#include "nix/util/experimental-features.hh"
#include "nix/util/util.hh"

#include <algorithm>

namespace nix {

Machine::Machine(
    const std::string & storeUri,
    decltype(systemTypes) systemTypes,
    decltype(sshKey) sshKey,
    decltype(maxJobs) maxJobs,
    decltype(speedFactor) speedFactor,
    decltype(supportedFeatures) supportedFeatures,
    decltype(mandatoryFeatures) mandatoryFeatures,
    decltype(sshPublicHostKey) sshPublicHostKey)
    : storeUri(
          StoreReference::parse(
              // Backwards compatibility: if the URI is schemeless, is not a path,
              // and is not one of the special store connection words, prepend
              // ssh://.
              storeUri.find("://") != std::string::npos || storeUri.find("/") != std::string::npos || storeUri == "auto"
                      || storeUri == "daemon" || storeUri == "local" || hasPrefix(storeUri, "auto?")
                      || hasPrefix(storeUri, "daemon?") || hasPrefix(storeUri, "local?") || hasPrefix(storeUri, "?")
                  ? storeUri
                  : "ssh://" + storeUri))
    , systemTypes(systemTypes)
    , sshKey(sshKey)
    , maxJobs(maxJobs)
    , speedFactor(speedFactor == 0.0f ? 1.0f : speedFactor)
    , supportedFeatures(supportedFeatures)
    , mandatoryFeatures(mandatoryFeatures)
    , sshPublicHostKey(sshPublicHostKey)
{
    if (speedFactor < 0.0)
        throw UsageError("speed factor must be >= 0");
}

bool Machine::systemSupported(const std::string & system) const
{
    return system == "builtin" || (systemTypes.count(system) > 0);
}

bool Machine::allSupported(const StringSet & features) const
{
    return std::all_of(features.begin(), features.end(), [&](const std::string & feature) {
        // Extract feature name without quantity (e.g., "mem:32" -> "mem")
        auto colonPos = feature.find(':');
        std::string featureName = (colonPos != std::string::npos) ? feature.substr(0, colonPos) : feature;
        return supportedFeatures.count(featureName) || mandatoryFeatures.count(featureName);
    });
}

bool Machine::mandatoryMet(const StringSet & features) const
{
    return std::all_of(mandatoryFeatures.begin(), mandatoryFeatures.end(), [&](const std::string & feature) {
        // Extract feature name without quantity
        auto colonPos = feature.find(':');
        std::string featureName = (colonPos != std::string::npos) ? feature.substr(0, colonPos) : feature;
        
        // Check if the feature (without quantity) is in the required features
        bool found = false;
        for (const auto & reqFeature : features) {
            auto reqColonPos = reqFeature.find(':');
            std::string reqFeatureName = (reqColonPos != std::string::npos) ? reqFeature.substr(0, reqColonPos) : reqFeature;
            if (reqFeatureName == featureName) {
                found = true;
                break;
            }
        }
        return found;
    });
}

StoreReference Machine::completeStoreReference() const
{
    auto storeUri = this->storeUri;

    auto * generic = std::get_if<StoreReference::Specified>(&storeUri.variant);

    if (generic && generic->scheme == "ssh") {
        storeUri.params["max-connections"] = "1";
        storeUri.params["log-fd"] = "4";
    }

    if (generic && (generic->scheme == "ssh" || generic->scheme == "ssh-ng")) {
        if (sshKey != "")
            storeUri.params["ssh-key"] = sshKey;
        if (sshPublicHostKey != "")
            storeUri.params["base64-ssh-public-host-key"] = sshPublicHostKey;
    }

    {
        auto & fs = storeUri.params["system-features"];
        auto append = [&](auto feats) {
            for (auto & f : feats) {
                if (fs.size() > 0)
                    fs += ' ';
                fs += f;
            }
        };
        append(supportedFeatures);
        append(mandatoryFeatures);
    }

    return storeUri;
}

ref<Store> Machine::openStore() const
{
    return nix::openStore(completeStoreReference());
}

static std::vector<std::string> expandBuilderLines(const std::string & builders)
{
    std::vector<std::string> result;
    for (auto line : tokenizeString<std::vector<std::string>>(builders, "\n")) {
        line.erase(std::find(line.begin(), line.end(), '#'), line.end());
        for (auto entry : tokenizeString<std::vector<std::string>>(line, ";")) {
            entry = trim(entry);

            if (entry.empty()) {
                // skip blank entries
            } else if (entry[0] == '@') {
                const std::string path = trim(std::string_view{entry}.substr(1));
                std::string text;
                try {
                    text = readFile(path);
                } catch (const SysError & e) {
                    if (e.errNo != ENOENT)
                        throw;
                    debug("cannot find machines file '%s'", path);
                    continue;
                }

                const auto entrys = expandBuilderLines(text);
                result.insert(end(result), begin(entrys), end(entrys));
            } else {
                result.emplace_back(entry);
            }
        }
    }
    return result;
}

/**
 * Parse a feature string that may contain a resource quantity (e.g., "mem:32", "gpu:2").
 * Returns a pair of (feature_name, quantity). If no quantity is specified, quantity is 0.
 */
static std::pair<std::string, unsigned int> parseFeatureWithQuantity(const std::string & feature)
{
    auto colonPos = feature.find(':');
    if (colonPos == std::string::npos) {
        return {feature, 0};
    }
    
    std::string featureName = feature.substr(0, colonPos);
    std::string quantityStr = feature.substr(colonPos + 1);
    
    auto quantity = string2Int<unsigned int>(quantityStr);
    if (!quantity) {
        throw FormatError("invalid resource quantity in feature '%s'", feature);
    }
    
    return {featureName, *quantity};
}

/**
 * Parse a set of features, extracting resource quantities when present.
 * Returns a pair of (features without quantities, map of feature name to quantity).
 */
static std::pair<StringSet, std::map<std::string, unsigned int>>
parseFeaturesWithQuantities(const StringSet & features)
{
    StringSet featureNames;
    std::map<std::string, unsigned int> quantities;
    
    for (const auto & feature : features) {
        auto [name, quantity] = parseFeatureWithQuantity(feature);
        featureNames.insert(name);
        if (quantity > 0) {
            quantities[name] = quantity;
        }
    }
    
    return {featureNames, quantities};
}

static Machine parseBuilderLine(const StringSet & defaultSystems, const std::string & line)
{
    const auto tokens = tokenizeString<std::vector<std::string>>(line);

    auto isSet = [&](size_t fieldIndex) {
        return tokens.size() > fieldIndex && tokens[fieldIndex] != "" && tokens[fieldIndex] != "-";
    };

    auto parseUnsignedIntField = [&](size_t fieldIndex) {
        const auto result = string2Int<unsigned int>(tokens[fieldIndex]);
        if (!result) {
            throw FormatError(
                "bad machine specification: failed to convert column #%lu in a row: '%s' to 'unsigned int'",
                fieldIndex,
                line);
        }
        return result.value();
    };

    auto parseFloatField = [&](size_t fieldIndex) {
        const auto result = string2Float<float>(tokens[fieldIndex]);
        if (!result) {
            throw FormatError(
                "bad machine specification: failed to convert column #%lu in a row: '%s' to 'float'", fieldIndex, line);
        }
        return result.value();
    };

    auto ensureBase64 = [&](size_t fieldIndex) {
        const auto & str = tokens[fieldIndex];
        try {
            base64::decode(str);
        } catch (FormatError & e) {
            e.addTrace({}, "while parsing machine specification at a column #%lu in a row: '%s'", fieldIndex, line);
            throw;
        }
        return str;
    };

    if (!isSet(0))
        throw FormatError(
            "bad machine specification: store URL was not found at the first column of a row: '%s'", line);

    // Parse features with potential resource quantities
    auto rawSupportedFeatures = isSet(5) ? tokenizeString<StringSet>(tokens[5], ",") : StringSet{};
    auto rawMandatoryFeatures = isSet(6) ? tokenizeString<StringSet>(tokens[6], ",") : StringSet{};
    
    auto [supportedFeatures, supportedQuantities] = parseFeaturesWithQuantities(rawSupportedFeatures);
    auto [mandatoryFeatures, mandatoryQuantities] = parseFeaturesWithQuantities(rawMandatoryFeatures);

    // TODO use designated initializers, once C++ supports those with
    // custom constructors.
    Machine machine{
        // `storeUri`
        tokens[0],
        // `systemTypes`
        isSet(1) ? tokenizeString<StringSet>(tokens[1], ",") : defaultSystems,
        // `sshKey`
        isSet(2) ? tokens[2] : "",
        // `maxJobs`
        isSet(3) ? parseUnsignedIntField(3) : 1U,
        // `speedFactor`
        isSet(4) ? parseFloatField(4) : 1.0f,
        // `supportedFeatures`
        supportedFeatures,
        // `mandatoryFeatures`
        mandatoryFeatures,
        // `sshPublicHostKey`
        isSet(7) ? ensureBase64(7) : ""};
    
    machine.supportedFeatureQuantities = supportedQuantities;
    machine.mandatoryFeatureQuantities = mandatoryQuantities;
    
    return machine;
}

static Machines parseBuilderLines(const StringSet & defaultSystems, const std::vector<std::string> & builders)
{
    Machines result;
    std::transform(builders.begin(), builders.end(), std::back_inserter(result), [&](auto && line) {
        return parseBuilderLine(defaultSystems, line);
    });
    return result;
}

Machines Machine::parseConfig(const StringSet & defaultSystems, const std::string & s)
{
    const auto builderLines = expandBuilderLines(s);
    return parseBuilderLines(defaultSystems, builderLines);
}

Machines getMachines()
{
    return Machine::parseConfig({settings.thisSystem}, settings.builders);
}

} // namespace nix
