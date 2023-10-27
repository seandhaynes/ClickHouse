#include <IO/ReadHelpers.h>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <tuple>
#include <vector>
#include <type_traits>
#include <unordered_map>
#include <Processors/Formats/Impl/NpyRowInputFormat.h>
#include <Formats/FormatFactory.h>
#include <Formats/EscapingRuleUtils.h>
#include <DataTypes/Serializations/SerializationNullable.h>
#include <DataTypes/DataTypeString.h>
#include <Common/assert_cast.h>
#include <Common/typeid_cast.h>
#include <Common/Exception.h>
#include <Columns/ColumnFixedString.h>
#include <Core/TypeId.h>
#include <Core/Types_fwd.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnArray.h>
#include <Columns/ColumnsNumber.h>
#include <Storages/IStorage.h>
#include <Columns/IColumn.h>
#include <Core/Field.h>
#include <Core/NamesAndTypes.h>
#include <DataTypes/DataTypeArray.h>
#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/IDataType.h>
#include <DataTypes/Serializations/ISerialization.h>
#include <IO/ReadBuffer.h>
#include <IO/WriteHelpers.h>
#include <Processors/Formats/IRowInputFormat.h>
#include <base/types.h>
#include <boost/algorithm/string/split.hpp>


namespace DB
{

namespace ErrorCodes
{
    extern const int INCORRECT_DATA;
    extern const int BAD_ARGUMENTS;
    extern const int LOGICAL_ERROR;
    extern const int UNKNOWN_TYPE;
    extern const int ILLEGAL_COLUMN;
}

namespace
{

DataTypePtr getDataTypeFromNumpyType(const std::shared_ptr<NumpyDataType> & numpy_type)
{
    switch (numpy_type->getTypeIndex())
    {
        case NumpyDataTypeIndex::Int8:
            return std::make_shared<DataTypeInt8>();
        case NumpyDataTypeIndex::Int16:
            return std::make_shared<DataTypeInt16>();
        case NumpyDataTypeIndex::Int32:
            return std::make_shared<DataTypeInt32>();
        case NumpyDataTypeIndex::Int64:
            return std::make_shared<DataTypeInt64>();
        case NumpyDataTypeIndex::UInt8:
            return std::make_shared<DataTypeUInt8>();
        case NumpyDataTypeIndex::UInt16:
            return std::make_shared<DataTypeUInt16>();
        case NumpyDataTypeIndex::UInt32:
            return std::make_shared<DataTypeUInt32>();
        case NumpyDataTypeIndex::UInt64:
            return std::make_shared<DataTypeUInt64>();
        case NumpyDataTypeIndex::Float32:
            return std::make_shared<DataTypeFloat32>();
        case NumpyDataTypeIndex::Float64:
            return std::make_shared<DataTypeFloat64>();
        case NumpyDataTypeIndex::String:
            return std::make_shared<DataTypeString>();
        case NumpyDataTypeIndex::Unicode:
            return std::make_shared<DataTypeString>();
    }
    throw Exception(ErrorCodes::UNKNOWN_TYPE, "Numpy type {} is not supported", magic_enum::enum_name(numpy_type->getTypeIndex()));
}

DataTypePtr createNestedArrayType(const DataTypePtr & nested_type, size_t depth)
{
    DataTypePtr result_type = nested_type;
    assert(depth > 0);
    if (depth > 1)
    {
        for (size_t i = 0; i < depth - 1; ++i)
            result_type = std::make_shared<DataTypeArray>(std::move(result_type));
    }
    return result_type;
}

size_t parseTypeSize(const std::string type)
{
    try
    {
        size_t size = std::stoi(type);
        return size;
    }
    catch (...)
    {
        throw Exception(ErrorCodes::INCORRECT_DATA, "Invalid data type");
    }
}

std::shared_ptr<NumpyDataType> parseType(String type)
{
    /// Parse endianess
    NumpyDataType::Endianness endianess;
    if (type[0] == '<')
        endianess = NumpyDataType::Endianness::LITTLE;
    else if (type[1] == '>')
        endianess = NumpyDataType::Endianness::BIG;
    else if (type[0] == '|')
        endianess = NumpyDataType::Endianness::NONE;
    else
      throw Exception(ErrorCodes::BAD_ARGUMENTS, "Wrong header data");

    /// Parse type
    if (type[1] == 'i')
        return std::make_shared<NumpyDataTypeInt>(endianess, parseTypeSize(type.substr(2)), true);
    else if (type[1] == 'b')
        return std::make_shared<NumpyDataTypeInt>(endianess, parseTypeSize(type.substr(2)), false);
    else if (type[1] == 'u')
        return std::make_shared<NumpyDataTypeInt>(endianess, parseTypeSize(type.substr(2)), false);
    else if (type[1] == 'f')
        return std::make_shared<NumpyDataTypeFloat>(endianess, parseTypeSize(type.substr(2)));
    else if (type[1] == 'S')
        return std::make_shared<NumpyDataTypeString>(endianess, parseTypeSize(type.substr(2)));
    else if (type[1] == 'U')
        return std::make_shared<NumpyDataTypeUnicode>(endianess, parseTypeSize(type.substr(2)));
    else if (type[1] == 'c')
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "ClickHouse doesn't support complex numeric type");
    else if (type[1] == 'O')
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "ClickHouse doesn't support object types");
    else
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Error while parsing data type");
}

std::vector<int> parseShape(String shape_string)
{
    shape_string.erase(std::remove(shape_string.begin(), shape_string.end(), '('), shape_string.end());
    shape_string.erase(std::remove(shape_string.begin(), shape_string.end(), ')'), shape_string.end());
    std::vector<std::string> result_str;
    boost::split(result_str, shape_string, boost::is_any_of(","));

    std::vector<int> shape;
    if (result_str[result_str.size()-1].empty())
        result_str.pop_back();
    shape.reserve(result_str.size());
    for (const String& item : result_str)
        shape.push_back(std::stoi(item));
    return shape;
}

NumpyHeader parseHeader(ReadBuffer &buf)
{
    /// Check magic bytes
    const char * magic_string = "\x93NUMPY";
    assertString(magic_string, buf);

    /// Read npy version.
    UInt8 version_major;
    UInt8 version_minor;
    readBinary(version_major, buf);
    readBinary(version_minor, buf);

    /// Read header length.
    UInt32 header_length;
    /// In v1 header length is 2 bytes, in v2 - 4 bytes.
    if (version_major == 1)
    {
        UInt16 header_length_u16;
        readBinaryLittleEndian(header_length_u16, buf);
        header_length = header_length_u16;
    }
    else
    {
        readBinaryLittleEndian(header_length, buf);
    }

    /// Remember current count of read bytes to skip remaining
    /// bytes in header when we find all required fields.
    size_t header_start = buf.count();

    /// Start parsing header.
    String shape;
    String descr;

    assertChar('{', buf);
    skipWhitespaceIfAny(buf);
    bool first = true;
    while (!checkChar('}', buf))
    {
        /// Skip delimiter between key-value pairs.
        if (!first)
        {
            skipWhitespaceIfAny(buf);
        }
        else
        {
            first = false;
        }

        /// Read map key.
        String key;
        readQuotedString(key, buf);
        assertChar(':', buf);
        skipWhitespaceIfAny(buf);
        /// Read map value.
        String value;
        readQuotedField(value, buf);
        assertChar(',', buf);
        skipWhitespaceIfAny(buf);

        if (key == "descr")
            descr = value;
        else if (key == "fortran_order")
        {
            if (value != "false")
                throw Exception(ErrorCodes::INCORRECT_DATA, "Fortran order is not supported");
        }
        else if (key == "shape")
            shape = value;
    }

    if (shape.empty() || descr.empty())
        throw Exception(ErrorCodes::INCORRECT_DATA, "npy file header doesn't contain required field 'shape' or 'descr'");

    size_t read_bytes = buf.count() - header_start;
    if (read_bytes > header_length)
        throw Exception(ErrorCodes::INCORRECT_DATA, "Header size is incorrect");

    /// Ignore remaining header data.
    buf.ignore(header_length - read_bytes);

    if (descr[0] == '\'')
        descr = descr.substr(1, descr.length() - 1);
    if (descr[descr.length() - 1] == '\'')
        descr = descr.substr(0, descr.length() - 1);

    if (shape[0] == '\'')
        shape = shape.substr(1, shape.length() - 1);
    if (shape[shape.length() - 1] == '\'')
        shape = shape.substr(0, shape.length() - 1);

    NumpyHeader res;
    res.shape = parseShape(shape);
    res.numpy_type = parseType(descr);

    return res;
}

DataTypePtr getNestedType(DataTypePtr type)
{
    String a = type->getName();
    while (const auto * temp_type = typeid_cast<const DataTypeArray *>(type.get()))
    {
        type = temp_type->getNestedType();
        a = type->getName();
    }

    return type;
}
}

void NpyRowInputFormat::readPrefix()
{
    header = parseHeader(*in);
}

NpyRowInputFormat::NpyRowInputFormat(ReadBuffer & in_, Block header_, Params params_)
    : IRowInputFormat(std::move(header_), in_, std::move(params_))
{
    auto types = getPort().getHeader().getDataTypes();
    if (types.size() != 1)
        throw Exception(ErrorCodes::BAD_ARGUMENTS, "Unexpected number of columns for Npy input format, expected one column, got {} columns", types.size());
    nested_type = getNestedType(types[0]);
}

void NpyRowInputFormat::readRows(MutableColumns & columns)
{
    auto & column = columns[0];
    IColumn * current_column = column.get();
    size_t elements_in_current_column = 1;
    for (size_t i = 1; i != header.shape.size(); ++i)
    {
        auto & array_column = assert_cast<ColumnArray &>(*current_column);
        /// Fill offsets of array columns.
        for (size_t j = 0; j != elements_in_current_column; ++j)
            array_column.getOffsets().push_back(array_column.getOffsets().back() + header.shape[i]);
        current_column = &array_column.getData();
        elements_in_current_column *= header.shape[i];
    }

    for (size_t i = 0; i != elements_in_current_column; ++i)
        chooseType(current_column);
}

template <typename ColumnValue, typename DataValue>
void NpyRowInputFormat::readBinaryValueAndInsert(MutableColumnPtr column, NumpyDataType::Endianness endianess)
{
    DataValue value;
    if (endianess == NumpyDataType::Endianness::BIG)
        readBinaryBigEndian(value, *in);
    else
        readBinaryLittleEndian(value, *in);
    assert_cast<ColumnVector<ColumnValue> &>(*column).insertValue(static_cast<ColumnValue>(value));
}

void NpyRowInputFormat::readStringBinaryAndInsert(MutableColumnPtr column, size_t size, bool is_fixed)
{
    if (is_fixed)
    {
        auto & fixed_string_column = assert_cast<ColumnFixedString &>(*column);
        size_t n = fixed_string_column.getN();
        if (size > n)
            throw Exception(ErrorCodes::TOO_LARGE_STRING_SIZE, "Too large string for FixedString column");
        auto & data = fixed_string_column.getChars();

        size_t old_size = data.size();
        data.resize_fill(old_size + n);

        String tmp;
        tmp.resize(size);

        in->readStrict(tmp.data(), size);
        tmp.erase(std::remove(tmp.begin(), tmp.end(), '\0'), tmp.end());
        fixed_string_column.insertData(tmp.c_str(), tmp.size());
    }
    else
    {
        auto & column_string = assert_cast<ColumnString &>(*column);
        String tmp;

        tmp.resize(size);
        in->readStrict(tmp.data(), size);
        tmp.erase(std::remove(tmp.begin(), tmp.end(), '\0'), tmp.end());
        column_string.insertData(tmp.c_str(), tmp.size());
    }
}

template <typename T>
void NpyRowInputFormat::readAndInsertInteger(IColumn * column, const DataTypePtr & data_type, const NumpyDataType & npy_type)
{
    switch (npy_type.getTypeIndex())
    {
        case NumpyDataTypeIndex::Int8: readBinaryValueAndInsert<T, UInt8>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::Int16: readBinaryValueAndInsert<T, UInt16>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::Int32: readBinaryValueAndInsert<T, UInt32>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::Int64: readBinaryValueAndInsert<T, UInt64>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::UInt8: readBinaryValueAndInsert<T, UInt8>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::UInt16: readBinaryValueAndInsert<T, UInt16>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::UInt32: readBinaryValueAndInsert<T, UInt32>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::UInt64: readBinaryValueAndInsert<T, UInt64>(column->getPtr(), npy_type.getEndianness()); break;
        default:
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Cannot insert data type into column with type {}",
                        data_type->getName());
    }
}

template <typename T>
void NpyRowInputFormat::readAndInsertFloat(IColumn * column, const DataTypePtr & data_type, const NumpyDataType & npy_type)
{
    switch (npy_type.getTypeIndex())
    {
        case NumpyDataTypeIndex::Float32: readBinaryValueAndInsert<T, Float32>(column->getPtr(), npy_type.getEndianness()); break;
        case NumpyDataTypeIndex::Float64: readBinaryValueAndInsert<T, Float64>(column->getPtr(), npy_type.getEndianness()); break;
        default:
            throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Cannot insert data type into column with type {}",
                        data_type->getName());
    }
}

template <typename T>
void NpyRowInputFormat::readAndInsertString(MutableColumnPtr column, const DataTypePtr & data_type, const NumpyDataType & npy_type, bool is_fixed)
{
    size_t size;
    
    if (npy_type.getTypeIndex() == NumpyDataTypeIndex::String)
        size = npy_type.getSize();
    else if (npy_type.getTypeIndex() == NumpyDataTypeIndex::Unicode)
        size = npy_type.getSize() * 4;
    else
        throw Exception(ErrorCodes::ILLEGAL_COLUMN, "Cannot insert data type into column with type {}",
                        data_type->getName());

    readStringBinaryAndInsert(column->getPtr(), size, is_fixed);
}

void NpyRowInputFormat::chooseType(IColumn * column)
{
    switch (nested_type->getTypeId())
    {
        case TypeIndex::UInt8: readAndInsertInteger<UInt8>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::UInt16: readAndInsertInteger<UInt16>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::UInt32: readAndInsertInteger<UInt32>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::UInt64: readAndInsertInteger<UInt64>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::Int8: readAndInsertInteger<Int8>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::Int16: readAndInsertInteger<Int16>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::Int32: readAndInsertInteger<Int32>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::Int64: readAndInsertInteger<Int64>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::Float32: readAndInsertFloat<Float32>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::Float64: readAndInsertFloat<Float64>(column, nested_type, *header.numpy_type); break;
        case TypeIndex::String: readAndInsertString<String>(column->getPtr(), nested_type, *header.numpy_type, false); break;
        case TypeIndex::FixedString: readAndInsertString<String>(column->getPtr(), nested_type, *header.numpy_type, true); break;
        default:
            throw Exception(ErrorCodes::UNKNOWN_TYPE, "Incorrect type of Npy");
    }
}

bool NpyRowInputFormat::readRow(MutableColumns & columns, RowReadExtension &  /*ext*/)
{
    if (in->eof())
        return false;
    else
        readRows(columns);
    return true;
}

void NpyRowInputFormat::resetParser()
{
    IRowInputFormat::resetParser();
}

NpySchemaReader::NpySchemaReader(ReadBuffer & in_)
    : ISchemaReader(in_) {}

NamesAndTypesList NpySchemaReader::readSchema()
{
    if (first_row)
    {
        skipBOMIfExists(in);
        first_row = false;
    }

    if (in.eof())
    {
        return {};
    }

    if (*in.position() == '\n')
    {
        ++in.position();
        return {};
    }

    NumpyHeader header = parseHeader(in);
    DataTypePtr nested_type = getDataTypeFromNumpyType(header.numpy_type);
    DataTypePtr result_type = createNestedArrayType(nested_type, header.shape.size());

    return {{"array", result_type}};
}

void registerInputFormatNpy(FormatFactory & factory)
{
    factory.registerInputFormat("Npy", [](
        ReadBuffer & buf,
        const Block & sample,
        IRowInputFormat::Params params,
        const FormatSettings &)
    {
        return std::make_shared<NpyRowInputFormat>(buf, sample, std::move(params));
    });

    factory.markFormatSupportsSubsetOfColumns("Npy");
}
void registerNpySchemaReader(FormatFactory & factory)
{
    factory.registerSchemaReader("Npy", [](ReadBuffer & buf, const FormatSettings &)
    {
        return std::make_shared<NpySchemaReader>(buf);
    });
}

}
