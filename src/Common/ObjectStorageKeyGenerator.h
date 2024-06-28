#pragma once

#include <memory>
#include "ObjectStorageKey.h"

namespace DB
{

class IObjectStorageKeysGenerator
{
public:
    virtual ~IObjectStorageKeysGenerator() = default;

    /// Generates an object storage key based on a path in the virtual filesystem.
    virtual ObjectStorageKey generate(const String & path, bool is_directory, const std::optional<String> & key_prefix) const = 0;
};

using ObjectStorageKeysGeneratorPtr = std::shared_ptr<IObjectStorageKeysGenerator>;

ObjectStorageKeysGeneratorPtr createObjectStorageKeysGeneratorAsIsWithPrefix(String key_prefix);
ObjectStorageKeysGeneratorPtr createObjectStorageKeysGeneratorByPrefix(String key_prefix);
ObjectStorageKeysGeneratorPtr createObjectStorageKeysGeneratorByTemplate(String key_template);

}
