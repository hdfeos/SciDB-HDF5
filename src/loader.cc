#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifndef H5_NO_NAMESPACE
#ifndef H5_NO_STD
    using std::cout;
    using std::endl;
#endif  // H5_NO_STD
#endif

#ifndef LOADER_HH
#include "loader.hh"
#endif

const H5std_string FILE_NAME( "sxrcom10-r0232.h5" );

const int Loader::OneDim::UNLIMITED = -1;
const int Loader::OneDim::UNKNOWN = -2;


const char* outFNameSchema = "/tmp/scidbSchema.sql";
const char* outFNameData   = "/tmp/scidbData.sql";


// FIXME: variable length strings

// FIXME big/small endian

// ============================================================================
// ============================================================================

std::ostream&
operator<<(std::ostream& os, const Loader::OneAttr& o) {
    os << "{" << Loader::print_H5T_class_t_Name(o.type)
       << ", tS=" << o.tS 
       << ", tN=" << o.tN 
       << ", predT=" << Loader::printPredType(*(o.predType)) 
       << ", inNA=" << o.inNA
       << ", " << o.attrName;
    if ( o.type == H5T_ENUM ) {
        std::vector<std::pair<std::string, long> >::const_iterator itr;
        for ( itr=o.enumMembers.begin(); itr!=o.enumMembers.end(); ++itr ) {
            os << " " << itr->first << ":" << itr->second;
        }
    }
    return os << "}";
}

// ============================================================================
// ============================================================================

std::ostream&
operator<<(std::ostream& os, const Loader::OneDim& o) {
    return os << "[" << o.d1 << ":" << o.d2 << ",n=" << o.curNElems << "]";
}

// ============================================================================
// ============================================================================

Loader::Loader() {
    _attr.clear();
    _dim.clear();

    std::remove(outFNameSchema);
    std::remove(outFNameData);
}

// ============================================================================
// ============================================================================

// recursively scan all groups in the file and process all data sets
void
Loader::doOneGroup(const std::string& objName, 
                   H5G_obj_t objType, 
                   const std::string& prefix, 
                   H5File file) {
    std::string thePrefix = prefix;
    int len = thePrefix.length();
    char c = thePrefix[len-1];
    if (  len > 0 && c != '/' ) {
        thePrefix += "/";
    }
    thePrefix += objName;
    if ( objType == H5G_GROUP ) {
        Group g = file.openGroup(thePrefix);
        int i, n = g.getNumObjs();
        for (i=0 ; i<n ; i++) {
            H5G_obj_t t = g.getObjTypeByIdx(i);
            const H5std_string gN = g.getObjnameByIdx(i);
            doOneGroup(gN, t, thePrefix, file);
        }
    } else if ( objType == H5G_DATASET ) {
        processDataSet(thePrefix);
    } else {
        cout << "Can't handle object type " << objType << endl;
    }
}

// ============================================================================
// ============================================================================

void
Loader::processDataSet(const std::string & dataSetName) {
    cout << "\nProcessing " << dataSetName << endl;
    _attr.clear();
    _dim.clear();
    
    H5File file( FILE_NAME, H5F_ACC_RDONLY );

    DataSet dataSet = file.openDataSet(dataSetName);
    H5T_class_t type_class = dataSet.getTypeClass();

    CompType compType;

    // handle "compound { vlen { compound {???} } }": 
    // turn it into extra dimension
    if ( type_class == H5T_COMPOUND ) {
        CompType ct = dataSet.getCompType();
        if ( ct.getNmembers() == 1  && 
             ct.getMemberDataType(0).getClass() == H5T_VLEN ) {
            // add dimension
            _dim.push_back(OneDim(0, OneDim::UNLIMITED, OneDim::UNKNOWN));
            // jump to the inside compound
            VarLenType vt = ct.getMemberVarLenType(0);
            DataType dt = vt.getSuper();
            type_class = dt.getClass();
            if ( type_class == H5T_COMPOUND ) {
                CompType* p = reinterpret_cast<CompType*>(&dt);
                compType = *p;
            }
        } else {
            compType = dataSet.getCompType();
        }
    }
    if ( type_class == H5T_INTEGER ) {
        addAttrIntType(dataSet.getIntType(), "");
    } else if ( type_class == H5T_FLOAT ) {
        addAttrFloatType(dataSet.getFloatType(), "");
    } else if ( type_class == H5T_COMPOUND ) {
        processCompoundType(compType, "");
    } else if ( type_class = H5T_ARRAY ) {
        ArrayType arrayType = dataSet.getArrayType();
        processArrayType(arrayType);
    } else {
        cout << "Unexpected type " << print_H5T_class_t_Name(type_class) 
             << endl;
        throw(1);
    }
    hsize_t curDims = 1;     // default for scalar dataSpace
    DataSpace dataSpace = dataSet.getSpace();
    if ( dataSpace.isSimple() ) {
        hsize_t maxDims = 1; // maxDims is not set for scalar dataSpace,
                             // so set it to 1
        dataSpace.getSimpleExtentDims(&curDims, &maxDims);
        OneDim oneDim(0, maxDims, curDims);
        static unsigned long long MAXD = 100000000000000;
        if (maxDims > MAXD ) {
            oneDim.d2 = OneDim::UNLIMITED;
        }
        _dim.push_back(oneDim);
    } else {
        _dim.push_back(OneDim(0, 1, 1));
    }

    dumpSchema(dataSetName);
    dumpData(dataSetName, dataSet, type_class, curDims);
}

// ============================================================================
// ============================================================================

void
Loader::dumpSchema(const std::string & dataSetName) {
    std::ofstream f(outFNameSchema, std::ios::out | std::ios::app);
    f << "\nCREATE ARRAY " << covertDataSetNameToArrayName(dataSetName)
      << " (" << attributesToString() << "\n) "
      << "[" << dimensionsToString() << "];" << endl;
    f.close();
}

// ============================================================================
// ============================================================================

void
Loader::dumpData(const std::string& dataSetName,
                 DataSet& dataSet, 
                 H5T_class_t typeClass, 
                 hsize_t curDim) {
    if ( _dim.size() == 1 && typeClass == H5T_COMPOUND ) {
        dumpData_1dArray_compound(dataSetName, dataSet, typeClass, curDim);
    } else if ( _dim.size() > 1 && typeClass == H5T_ARRAY ) {
        dumpData_mdArray_nonCompound(dataSetName, dataSet, typeClass, curDim);
    } else {
        cout << "\n"
             << " // Skipping " << dataSetName << "\n"
             << " // Currently only 1d-array of compound type\n"
             << " // or multi-d array of simple type can be handled.\n"
             << " // Dimensions: " << dimensionsToString() 
             << ", type = " << print_H5T_class_t_Name(typeClass) << endl;
        return; // FIXME
    }
}
    
// ============================================================================
// ============================================================================

void
Loader::dumpData_1dArray_compound(const std::string& dataSetName,
                                  DataSet& dataSet, 
                                  H5T_class_t typeClass, 
                                  hsize_t curDim) {
    if ( _dim[0].d2 == 1 ) { // FIXME (skipping scalars)
        cout << "Skipping scalar" << endl;
        return;
    }
    size_t tS = 0;
    std::vector<OneAttr>::const_iterator itr;
    for ( itr=_attr.begin(); itr!=_attr.end(); ++itr ) {
        tS += itr->tS;
    }
    int n, curNElems = _dim[0].curNElems;

    char* buffer = new char[tS*curNElems];
    CompType compType(tS);
    size_t offset = 0;
    int nestedArrayDim = 0;
    for ( itr=_attr.begin(); itr!=_attr.end(); itr++ ) {
        if ( itr->inNA != "" ) {
            nestedArrayDim ++;
            continue;
        } else if ( nestedArrayDim > 0 ) {
            itr--; // went past end of nested array, go back
            if ( itr->type == H5T_STRING ) {
                hid_t strType = H5Tcopy(H5T_C_S1);
                if (itr->strIsVlstr) {
                    H5Tset_size(strType, H5T_VARIABLE);
                }
                else {
                    H5Tset_size(strType, itr->tS);
                }
                H5Tset_cset(strType, itr->strCset);
                H5Tset_strpad(strType, itr->strPad);
                hsize_t array_dim[] = {nestedArrayDim};
                id_t naType = H5Tarray_create(strType, 1, array_dim);
                compType.insertMember(itr->inNA, offset, naType);
            } else {
                cout << "not implemented" << endl;
                throw(1); // FIXME nested arrays of types other than STRING
            }
            offset += itr->tS * nestedArrayDim;
            nestedArrayDim = 0;
            continue;
        }
        if ( itr->type == H5T_STRING ) {
            hid_t strType = H5Tcopy(H5T_C_S1);
            if (itr->strIsVlstr) {
                H5Tset_size(strType, H5T_VARIABLE);
            } else {
                H5Tset_size(strType, itr->tS);
            }
            H5Tset_cset(strType, itr->strCset);
            H5Tset_strpad(strType, itr->strPad);
            compType.insertMember(itr->attrName, offset, strType);
        } else if ( itr->type == H5T_ENUM ) {
            hid_t enumType = H5Tenum_create(itr->predType->getId());
            //hid_t intType = H5Tcopy(itr->predType->getId());
            //EnumType enumType (intType);
            std::vector<std::pair<std::string, long> >::const_iterator eItr;
            for ( eItr=itr->enumMembers.begin(); 
                  eItr!=itr->enumMembers.end(); 
                  eItr++ ) {
                const char* n = eItr->first.c_str();
                uint16_t l = eItr->second;
                //enumType.insert(eItr->first, &l);
                H5Tenum_insert(enumType, n, &l);
            }
            compType.insertMember(itr->attrName, offset, enumType);
        } else {
            compType.insertMember(itr->attrName, offset, *(itr->predType));
        }
        offset += itr->tS;
        if ( itr->tN == "" ) {
            cout << "(1) Unable to determine type while dumping data for " 
                 << dataSetName << ", attr " << itr->attrName << endl;
            throw (1);
        }
    }

    dataSet.read((void*)buffer, compType);

    std::ofstream f(outFNameData, std::ios::out | std::ios::app);
    f << "\n\nData for " << dataSetName << endl;
    for ( n=0 ; n<curNElems ; n++ ) {
        std::ostringstream oss;
        oss << "#" << n << ": {";
        offset = n*tS;
        int i = 0;
        for ( itr=_attr.begin(); itr!=_attr.end(); ++itr ) {
            if ( i++ > 0 ) {
                oss << ", ";
            }
            if ( itr->tN == "INT8" ) {
                char* x = reinterpret_cast<char*>(&(buffer[offset]));
                int xx = (int) *x;
                oss << xx;
            } else if ( itr->tN == "UINT8" ) {
                unsigned char* x = 
                    reinterpret_cast<unsigned char*>(&(buffer[offset]));
                unsigned int xx = (unsigned int) *x;
                oss << xx;
            } else if ( itr->tN == "INT16" ) {
                int16_t* x = reinterpret_cast<int16_t*>(&(buffer[offset]));
                oss << *x;
            } else if ( itr->tN == "UINT16" ) {
                uint16_t* x = reinterpret_cast<uint16_t*>(&(buffer[offset]));
                oss << *x;
            } else if ( itr->tN == "INT32" ) {
                int32_t* x = reinterpret_cast<int32_t*>(&(buffer[offset]));
                oss << *x;
            } else if ( itr->tN == "UINT32" || itr->tN == "INT32" ) {
                uint32_t* x = reinterpret_cast<uint32_t*>(&(buffer[offset]));
                oss << *x;
            } else if ( itr->tN == "DOUBLE" ) {
                double* x = reinterpret_cast<double*>(&(buffer[offset]));
                oss << *x;
            } else if ( itr->type == H5T_STRING ) { // FIXME (reading string)
                char* x = new char [itr->tS+1];
                memset(x, 0, itr->tS+1);
                memcpy(x, &(buffer[offset]), itr->tS);
                oss << '"' << x<< '"' ;
            } else {
                cout << "(2) Unable to determine type while dumping data for " 
                     << dataSetName << ", attr " << itr->attrName << endl;
                throw (1);
            }
            offset += itr->tS;
        }
        oss << "}" << endl;
        f << oss.str();
        cout << oss.str();
        
    }
    delete [] buffer;
}

// ============================================================================
// ============================================================================

void
Loader::dumpData_mdArray_nonCompound(const std::string& dataSetName,
                                     DataSet& dataSet, 
                                     H5T_class_t typeClass, 
                                     hsize_t curDim) {
    // type to be read is a 2-d array 1024x1024
    // need to read it one at a time, in a loop, see above how many times
    // or get from the _dims

    // memSpace will be a 2-d array

    if ( _attr.size() > 1 ) {
        cout << "I can only handle simple types for multi-d arrays" << endl;
        throw(1);
    }
    // Calculate buffer size. Current logic: read one d-1 array at a time
    // Fail if it is > 128MB
    long bufferSize = _attr[0].tS;
    long nElemsInArray = 1;
    
    int i, rank = _dim.size()-1;
    std::vector< OneDim >::const_iterator itrD;
    hsize_t* dims = new hsize_t[rank];
    for ( i=0, itrD=_dim.begin() ; i<rank ; itrD++, i++ ) {
        int n = itrD->curNElems;
        bufferSize *= n;
        nElemsInArray *= n;
        dims[i] = n;
    }
    int nElemsLastDim = itrD->curNElems;
    if ( bufferSize > 128 * 1024 * 1024 ) {
        cout << "Array to read is too large" << endl;
        return;
    }
    hid_t arrayType = H5Tarray_create(_attr[0].predType->getId(), rank, dims);
    
    hsize_t dHsSize = 1 ;   // data hyperslab size, read one elem at a time
    hsize_t dHsOffs = 0;    // data hyperslab offset, gradually increment

    cout << "working with buffer size " << bufferSize 
         << ", will loop " << nElemsLastDim << " times" << endl;

    char* buffer = new char[bufferSize];
    
    DataSpace dataSpace = dataSet.getSpace();

    //DataSpace memSpace(nDims-1, mHsSize);
    //memSpace.selectHyperslab(H5S_SELECT_SET, mHsSize, mHsOffs);
    
    for (dHsOffs=0 ; dHsOffs<nElemsLastDim ; dHsOffs++ ) {
        cout << "hsize is " << dHsSize << ", offs is " << dHsOffs << endl;
        
        dataSpace.selectHyperslab(H5S_SELECT_SET, &dHsSize, &dHsOffs);

        memset(buffer, 0, bufferSize);
        if ( dHsOffs%1001 == 1000 ) cout << dHsOffs << endl;

        dataSet.read(buffer, arrayType, DataSpace::ALL, dataSpace);
        for ( i=0 ; i<nElemsInArray ; i++ ) {
            cout << *(reinterpret_cast<uint16_t*> (buffer+2)) << " ";
        }
        cout << endl;
        
    }
    delete [] buffer;
}

// ============================================================================
// ============================================================================

void
Loader::addAttrIntType(const DataType& dt, const H5std_string& mName) {
    OneAttr a;
    a.type = H5T_INTEGER;
    a.attrName = mName;

    int sI = sizeof(int);
    int sL1 = sizeof(long);
    int sL2 = sizeof(long long);
    if ( dt == PredType::STD_I8BE ) {
        a.predType = &PredType::STD_I8BE;     a.tN = "INT8";     a.tS=1;
    } else if ( dt == PredType::STD_I8LE ) {
        a.predType = &PredType::STD_I8LE;     a.tN = "INT8";     a.tS=1;
    } else if ( dt == PredType::STD_I16BE ) {
        a.predType = &PredType::STD_I16BE;    a.tN = "INT16";    a.tS=2;
    } else if ( dt == PredType::STD_I16LE ) {
        a.predType = &PredType::STD_I16LE;    a.tN = "INT16";    a.tS=2;
    } else if ( dt == PredType::STD_I32BE ) {
        a.predType = &PredType::STD_I32BE;    a.tN = "INT32";    a.tS=4;
    } else if ( dt == PredType::STD_I32LE ) {
        a.predType = &PredType::STD_I32LE;    a.tN = "INT32";    a.tS=4;
    } else if ( dt == PredType::STD_I64BE ) {
        a.predType = &PredType::STD_I64BE;    a.tN = "LONG";     a.tS=sL1;
    } else if ( dt == PredType::STD_I64LE ) {
        a.predType = &PredType::STD_I64LE;    a.tN = "LONG";     a.tS=sL1;
    } else if ( dt == PredType::STD_U8BE ) {
        a.predType = &PredType::STD_U8BE;     a.tN = "UINT8";    a.tS=1;
    } else if ( dt == PredType::STD_U8LE ) {
        a.predType = &PredType::STD_U8LE;     a.tN = "UINT8";    a.tS=1;
    } else if ( dt == PredType::STD_U16BE ) {
        a.predType = &PredType::STD_U16BE;    a.tN = "UINT16";   a.tS=2;
    } else if ( dt == PredType::STD_U16LE ) {
        a.predType = &PredType::STD_U16LE;    a.tN = "UINT16";   a.tS=2;
    } else if ( dt == PredType::STD_U32BE ) {
        a.predType = &PredType::STD_U32BE;    a.tN = "UINT32";   a.tS=4;
    } else if ( dt == PredType::STD_U32LE ) {
        a.predType = &PredType::STD_U32LE;    a.tN = "UINT32";   a.tS=4;
    } else if ( dt == PredType::STD_U64BE ) {
        a.predType = &PredType::STD_U64BE;    a.tN = "ULONG";    a.tS=sL1;
    } else if ( dt == PredType::STD_U64LE ) {
        a.predType = &PredType::STD_U64LE;    a.tN = "ULONG";    a.tS=sL1;
    } else if ( dt == PredType::NATIVE_SHORT ) {
        a.predType = &PredType::NATIVE_SHORT; a.tN = "INT16";    a.tS=2;
    } else if ( dt == PredType::NATIVE_USHORT ) {
        a.predType = &PredType::NATIVE_USHORT;a.tN = "UINT16";   a.tS=2;
    } else if ( dt == PredType::NATIVE_INT ) {
        a.predType = &PredType::NATIVE_INT;   a.tN = "INT";      a.tS=sI;
    } else if ( dt == PredType::NATIVE_UINT ) {
        a.predType = &PredType::NATIVE_UINT;  a.tN = "UINT";     a.tS=sI;
    } else if ( dt == PredType::NATIVE_LONG ) {
        a.predType = &PredType::NATIVE_LONG;  a.tN = "LONG";     a.tS=sL1;
    } else if ( dt == PredType::NATIVE_ULONG ) {
        a.predType = &PredType::NATIVE_ULONG; a.tN = "ULONG";    a.tS=sL1;
    } else if ( dt == PredType::NATIVE_LLONG ) {
        a.predType = &PredType::NATIVE_LLONG; a.tN = "LONGLONG"; a.tS=sL2;
    } else if ( dt == PredType::NATIVE_ULLONG ) {
        a.predType = &PredType::NATIVE_ULLONG;a.tN = "ULONGLONG";a.tS=sL2;
    } else {
        cout << "Can't determine type for INT attribute '" << mName << "'"
             << " (" << printPredType(dt) << ")" << endl;
        throw(1);
    }
    _attr.push_back(a);
}

// ============================================================================
// ============================================================================

void
Loader::addAttrFloatType(const DataType& dt, const H5std_string& mName) {
    OneAttr a;
    a.attrName = mName;
    a.type = H5T_FLOAT;

    int sF = sizeof(float);
    int sD = sizeof(double);
    if ( dt == PredType::IEEE_F32BE ) { 
        a.predType = &PredType::IEEE_F32BE;    a.tN = "FLOAT";  a.tS=sF;
    } else if ( dt == PredType::IEEE_F32LE ) {
        a.predType = &PredType::IEEE_F32LE;    a.tN = "FLOAT";  a.tS=sF;
    } else if ( dt == PredType::IEEE_F64BE ) {
        a.predType = &PredType::IEEE_F64BE;    a.tN = "DOUBLE"; a.tS=sD;
    } else if ( dt == PredType::IEEE_F64LE ) {
        a.predType = &PredType::IEEE_F64LE;    a.tN = "DOUBLE"; a.tS=sD;
    } else if ( dt == PredType::NATIVE_FLOAT ) {
        a.predType = &PredType::NATIVE_FLOAT;  a.tN = "FLOAT";  a.tS=sF;
    } else if ( dt == PredType::NATIVE_DOUBLE ) {
        a.predType = &PredType::NATIVE_DOUBLE; a.tN = "DOUBLE"; a.tS=sD;
    } else {
        cout << "Can't determine type for FLOAT attribute '" << mName << "'"
             << " (" << printPredType(dt) << ")" << endl;
        throw(1);
    }
    _attr.push_back(a);
}

// ============================================================================
// ============================================================================

void
Loader::addAttrStringType(const DataType& dt, const H5std_string& mName) {
    OneAttr a;
    a.type = H5T_STRING;
    a.attrName = mName;
    // FIXME: is there a better (c++-way) of dealing with strings?
    hid_t dt2 = H5Tcopy(dt.getId());
    a.tS         = H5Tget_size(dt2);
    a.strPad     = H5Tget_strpad(dt2);
    a.strCset    = H5Tget_cset(dt2);
    a.strIsVlstr = H5Tis_variable_str(dt2);

    hid_t strDt = H5Tcopy(H5T_C_S1);
    if (a.strIsVlstr) {
        H5Tset_size(strDt, H5T_VARIABLE);
    } else {
        H5Tset_size(strDt, a.tS);
    }
    H5Tset_cset(strDt, a.strCset);
    H5Tset_strpad(strDt, a.strPad);
    if (H5Tequal(dt2, strDt)) {
        a.predType = &PredType::C_S1;
    } else {
        cout << "Can't determine type for STRING attribute '" << mName << "'"
             << " (" << printPredType(dt) << ")" << endl;
        throw(1);
    }
    std::ostringstream oss;
    oss << "CHAR[" << a.tS << "]";
    a.tN = oss.str();
    _attr.push_back(a);

    H5Tclose(dt2);
    H5Tclose(strDt);
}

// ============================================================================
// ============================================================================

void
Loader::addAttrEnumType(const DataType& dt, const H5std_string& mName) {
    DataType sdt = dt.getSuper();
    
    OneAttr a;
    a.type = H5T_ENUM;
    a.attrName = mName;
    // FIXME (supporting enum correctly, fix one SciDB supports enum)
    if ( sdt == PredType::STD_U8LE ) {
        a.predType = &PredType::STD_U8LE;  a.tS = 1;a.tN = "UINT8";
    } else if ( sdt == PredType::STD_U16LE ) {
        a.predType = &PredType::STD_U16LE; a.tS = 2;a.tN = "UINT16";
    } else if ( sdt == PredType::STD_U32LE ) {
        a.predType = &PredType::STD_U32LE; a.tS = 4;a.tN = "UINT32";
    } else if ( sdt == PredType::STD_U64LE ) {
        a.predType = &PredType::STD_U64LE; a.tS = 8;a.tN = "UINT64";
    } else if ( sdt == PredType::STD_I16LE ) {
        a.predType = &PredType::STD_I16LE; a.tS = 2;a.tN = "INT16";
    } else if ( sdt == PredType::STD_I32LE ) {
        a.predType = &PredType::STD_I32LE; a.tS = 4;a.tN = "INT32";
    } else if ( sdt == PredType::STD_I64LE ) {
        a.predType = &PredType::STD_I64LE; a.tS = 8;a.tN = "INT64";
    } else {
        cout << "Can't determine type for ENUM attribute '" << mName << "'"
             << " (" << printPredType(sdt) << ")" << endl;
        throw(1);
    }
    // preserve enum members
    hid_t hId = dt.getId();
    EnumType enumType (hId);
    int i, n = enumType.getNmembers();
    for (i=0 ; i<n ; i++) {
        char* s = H5Tget_member_name(hId, i);
        std::pair<std::string, long> p(s, 0);
        if ( a.tS <= 2 ) {
            uint16_t v;
            H5Tget_member_value(hId, i, &v);
            p.second = v;
        } else if ( a.tS == 4 ) {
            uint32_t v;
            H5Tget_member_value(hId, i, &v);
            p.second = v;
        } else if ( a.tS == 8 ) {
            uint64_t v;
            H5Tget_member_value(hId, i, &v);
            p.second = v;
        }
        a.enumMembers.push_back(p);
    }
    _attr.push_back(a);
}

// ============================================================================
// ============================================================================

void
Loader::flattenArray(ArrayType& arrayType, const std::string& mName) {
    int i, nDims = arrayType.getArrayNDims();
    hsize_t dims;
    arrayType.getArrayDims(&dims);

    DataType dt = arrayType.getSuper();
    H5T_class_t dtc = dt.getClass();

    for (i = 0 ; i<nDims*dims ; i++) {
        std::ostringstream oss;
        oss << mName << "_" << i;
        if ( dtc == H5T_INTEGER ) {
            addAttrIntType(dt, oss.str());
        } else if (dtc == H5T_FLOAT ) {
            addAttrFloatType(dt, oss.str());
        } else if (dtc == H5T_STRING) {
            addAttrStringType(dt, oss.str());
        } else {
            throw 1;
        }
        _attr[_attr.size()-1].inNA = mName;
    }
}

// ============================================================================
// ============================================================================

std::string
Loader::attributesToString() const {
    std::stringstream oss;
    int i = 0;
    std::vector<OneAttr>::const_iterator itrA;
    for ( itrA=_attr.begin(); itrA!=_attr.end(); ++itrA, i++ ) {
        if ( i > 0 ) {
            oss << ",";
        }
        oss << "\n    " << itrA->tN;
        if ( itrA->attrName != "" ) {
            oss << "::" << itrA->attrName;
        }
    }
    return oss.str();
}

// ============================================================================
// ============================================================================

std::string
Loader::dimensionsToString() const {
    std::stringstream oss;
    int i = 0;
    std::vector< OneDim >::const_iterator itrD;
    for ( itrD=_dim.begin(); itrD!=_dim.end(); ++itrD, i++ ) {
        if ( i > 0 ) {
            oss << ", ";
        }
        oss << itrD->d1 << ":";
        if ( itrD->d2 == OneDim::UNLIMITED ) {
            oss << "*";
        } else {
            oss << itrD->d2;
        }
    }
    return oss.str();
}

// ============================================================================
// ============================================================================

std::string
Loader::print_H5T_class_t_Name(H5T_class_t t) 
{
    switch (t) {
      case H5T_INTEGER:  return "INTEGER";
      case H5T_FLOAT:    return "FLOAT";
      case H5T_TIME:     return "TIME";
      case H5T_STRING:   return "STRING";
      case H5T_BITFIELD: return "BITFIELD";
      case H5T_OPAQUE:   return "OPAQUE";
      case H5T_COMPOUND: return "COMPOUND";
      case H5T_REFERENCE:return "REFERENCE";
      case H5T_ENUM:     return "ENUM";
      case H5T_VLEN:     return "VLEN";
      case H5T_ARRAY:    return "ARRAY";
      case H5T_NCLASSES: return "NCLASSES";
      default: return "UNKNOWN";
    }
}

// ============================================================================
// ============================================================================

void
Loader::processCompoundType(const CompType& ct, 
                            const H5std_string& parentMName) {
    int i, n = ct.getNmembers();
    for (i=0 ; i<n ; i++) {
        H5std_string mName;
        if ( parentMName != "" ) {
            mName = parentMName;
            mName += "_";
            mName += ct.getMemberName(i);
        } else {
            mName = ct.getMemberName(i);
        }
        DataType dt = ct.getMemberDataType(i);
        H5T_class_t dtc = dt.getClass();
        std::string typeName;
        if ( dtc == H5T_COMPOUND ) {
            // Recursively flatten compound type inside compound type
            CompType ctNested = ct.getMemberCompType(i);
            processCompoundType(ctNested, mName);
        } else if (dtc == H5T_ARRAY) {
            ArrayType arrayType = ct.getMemberArrayType(i);
            flattenArray(arrayType, mName);
        } else {
            if (dtc == H5T_INTEGER) {
                addAttrIntType(dt, mName);
            } else if (dtc == H5T_FLOAT) {
                addAttrFloatType(dt, mName);
            } else if (dtc == H5T_STRING) {
                addAttrStringType(dt, mName);
            } else if (dtc == H5T_ENUM) {
                addAttrEnumType(dt, mName);
            } else { 
                cout << "Can't handle the type " 
                     << print_H5T_class_t_Name(dtc) << endl;
                throw(1);
            }
        }
    }
}

// ============================================================================
// ============================================================================

void
Loader::processArrayType(ArrayType& arrayType) {
    int i, nDims = arrayType.getArrayNDims();
    hsize_t dims;
    arrayType.getArrayDims(&dims);
    for (i=0 ; i<nDims ; i++) {
        _dim.push_back(OneDim(0, dims, dims));
    }
    DataType dt = arrayType.getSuper();
    H5T_class_t dtc = dt.getClass();
    if ( dtc == H5T_INTEGER ) {
        addAttrIntType(dt, "");
    } else if ( dtc == H5T_FLOAT ) {
        addAttrFloatType(dt, "");
    } else if (dtc == H5T_STRING) {
        addAttrStringType(dt, "");
    } else {
        throw 1;
    }
}

// ============================================================================
// ============================================================================

// Replaces '/', ':' and '.' with '_'. Also, remove leading '/'
std::string
Loader::covertDataSetNameToArrayName(const std::string & dataSetName) {
    std::string rep(dataSetName, 1);
    std::replace( rep.begin(), rep.end(), '/', '_' );
    std::replace( rep.begin(), rep.end(), ':', '_' );
    std::replace( rep.begin(), rep.end(), '.', '_' );
    return rep;
}

// ============================================================================
// ============================================================================

int main (void)
{
    try {
        //Exception::dontPrint();
        H5File file( FILE_NAME, H5F_ACC_RDONLY );
        std::string prefix = "";

        Loader loader;
        loader.doOneGroup("/", H5G_GROUP, prefix, file);

        // FIXME: dumping data for flattened arrays
        //loader.processDataSet("/Configure:0000/Epics::EpicsPv/EpicsArch.0:NoDevice.0/SXR:SPS:MPA:01:IN/data");

        // FIXME: multi-d arrays
        //loader.processDataSet("/Configure:0000/Run:0000/CalibCycle:0000/Camera::FrameV1/SxrBeamline.0:Opal1000.1/image");

        // DATASET "image" {
        //    DATATYPE  H5T_ARRAY { [1024][1024] H5T_STD_U16LE }
        //    DATASPACE  SIMPLE { ( 42247 ) / ( H5S_UNLIMITED ) }
        // }
        
    } catch( FileIException error ) {
        error.printError();
        return -1;
    } catch( DataSetIException error ) {
        error.printError();
        return -1;
    } catch( DataSpaceIException error ) {
        error.printError();
        return -1;
    } catch( DataTypeIException error ) {
        error.printError();
        return -1;
    }
    return 0;
}

// ============================================================================
// ============================================================================

std::string
Loader::printPredType(const DataType& dt) {
    
    if ( dt == PredType::C_S1 ) return "C_S1";
    else if ( dt == PredType::FORTRAN_S1 ) return "FORTRAN_S1";
    else if ( dt == PredType::STD_I8BE ) return "STD_I8BE";
    else if ( dt == PredType::STD_I8LE ) return "STD_I8LE";
    else if ( dt == PredType::STD_I16BE ) return "STD_I16BE";
    else if ( dt == PredType::STD_I16LE ) return "STD_I16LE";
    else if ( dt == PredType::STD_I32BE ) return "STD_I32BE";
    else if ( dt == PredType::STD_I32LE ) return "STD_I32LE";
    else if ( dt == PredType::STD_I64BE ) return "STD_I64BE";
    else if ( dt == PredType::STD_I64LE ) return "STD_I64LE";
    else if ( dt == PredType::STD_U8BE ) return "STD_U8BE";
    else if ( dt == PredType::STD_U8LE ) return "STD_U8LE";
    else if ( dt == PredType::STD_U16BE ) return "STD_U16BE";
    else if ( dt == PredType::STD_U16LE ) return "STD_U16LE";
    else if ( dt == PredType::STD_U32BE ) return "STD_U32BE";
    else if ( dt == PredType::STD_U32LE ) return "STD_U32LE";
    else if ( dt == PredType::STD_U64BE ) return "STD_U64BE";
    else if ( dt == PredType::STD_U64LE ) return "STD_U64LE";
    else if ( dt == PredType::STD_B8BE ) return "STD_B8BE";
    else if ( dt == PredType::STD_B8LE ) return "STD_B8LE";
    else if ( dt == PredType::STD_B16BE ) return "STD_B16BE";
    else if ( dt == PredType::STD_B16LE ) return "STD_B16LE";
    else if ( dt == PredType::STD_B32BE ) return "STD_B32BE";
    else if ( dt == PredType::STD_B32LE ) return "STD_B32LE";
    else if ( dt == PredType::STD_B64BE ) return "STD_B64BE";
    else if ( dt == PredType::STD_B64LE ) return "STD_B64LE";
    else if ( dt == PredType::STD_REF_OBJ ) return "STD_REF_OBJ";
    else if ( dt == PredType::STD_REF_DSETREG ) return "STD_REF_DSETREG";
    else if ( dt == PredType::IEEE_F32BE ) return "IEEE_F32BE";
    else if ( dt == PredType::IEEE_F32LE ) return "IEEE_F32LE";
    else if ( dt == PredType::IEEE_F64BE ) return "IEEE_F64BE";
    else if ( dt == PredType::IEEE_F64LE ) return "IEEE_F64LE";
    else if ( dt == PredType::UNIX_D32BE ) return "UNIX_D32BE";
    else if ( dt == PredType::UNIX_D32LE ) return "UNIX_D32LE";
    else if ( dt == PredType::UNIX_D64BE ) return "UNIX_D64BE";
    else if ( dt == PredType::UNIX_D64LE ) return "UNIX_D64LE";
    else if ( dt == PredType::INTEL_I8 ) return "INTEL_I8";
    else if ( dt == PredType::INTEL_I16 ) return "INTEL_I16";
    else if ( dt == PredType::INTEL_I32 ) return "INTEL_I32";
    else if ( dt == PredType::INTEL_I64 ) return "INTEL_I64";
    else if ( dt == PredType::INTEL_U8 ) return "INTEL_U8";
    else if ( dt == PredType::INTEL_U16 ) return "INTEL_U16";
    else if ( dt == PredType::INTEL_U32 ) return "INTEL_U32";
    else if ( dt == PredType::INTEL_U64 ) return "INTEL_U64";
    else if ( dt == PredType::INTEL_B8 ) return "INTEL_B8";
    else if ( dt == PredType::INTEL_B16 ) return "INTEL_B16";
    else if ( dt == PredType::INTEL_B32 ) return "INTEL_B32";
    else if ( dt == PredType::INTEL_B64 ) return "INTEL_B64";
    else if ( dt == PredType::INTEL_F32 ) return "INTEL_F32";
    else if ( dt == PredType::INTEL_F64 ) return "INTEL_F64";
    else if ( dt == PredType::ALPHA_I8 ) return "ALPHA_I8";
    else if ( dt == PredType::ALPHA_I16 ) return "ALPHA_I16";
    else if ( dt == PredType::ALPHA_I32 ) return "ALPHA_I32";
    else if ( dt == PredType::ALPHA_I64 ) return "ALPHA_I64";
    else if ( dt == PredType::ALPHA_U8 ) return "ALPHA_U8";
    else if ( dt == PredType::ALPHA_U16 ) return "ALPHA_U16";
    else if ( dt == PredType::ALPHA_U32 ) return "ALPHA_U32";
    else if ( dt == PredType::ALPHA_U64 ) return "ALPHA_U64";
    else if ( dt == PredType::ALPHA_B8 ) return "ALPHA_B8";
    else if ( dt == PredType::ALPHA_B16 ) return "ALPHA_B16";
    else if ( dt == PredType::ALPHA_B32 ) return "ALPHA_B32";
    else if ( dt == PredType::ALPHA_B64 ) return "ALPHA_B64";
    else if ( dt == PredType::ALPHA_F32 ) return "ALPHA_F32";
    else if ( dt == PredType::ALPHA_F64 ) return "ALPHA_F64";
    else if ( dt == PredType::MIPS_I8 ) return "MIPS_I8";
    else if ( dt == PredType::MIPS_I16 ) return "MIPS_I16";
    else if ( dt == PredType::MIPS_I32 ) return "MIPS_I32";
    else if ( dt == PredType::MIPS_I64 ) return "MIPS_I64";
    else if ( dt == PredType::MIPS_U8 ) return "MIPS_U8";
    else if ( dt == PredType::MIPS_U16 ) return "MIPS_U16";
    else if ( dt == PredType::MIPS_U32 ) return "MIPS_U32";
    else if ( dt == PredType::MIPS_U64 ) return "MIPS_U64";
    else if ( dt == PredType::MIPS_B8 ) return "MIPS_B8";
    else if ( dt == PredType::MIPS_B16 ) return "MIPS_B16";
    else if ( dt == PredType::MIPS_B32 ) return "MIPS_B32";
    else if ( dt == PredType::MIPS_B64 ) return "MIPS_B64";
    else if ( dt == PredType::MIPS_F32 ) return "MIPS_F32";
    else if ( dt == PredType::MIPS_F64 ) return "MIPS_F64";
    else if ( dt == PredType::NATIVE_CHAR ) return "NATIVE_CHAR";
    else if ( dt == PredType::NATIVE_INT ) return "NATIVE_INT";
    else if ( dt == PredType::NATIVE_FLOAT ) return "NATIVE_FLOAT";
    else if ( dt == PredType::NATIVE_SCHAR ) return "NATIVE_SCHAR";
    else if ( dt == PredType::NATIVE_UCHAR ) return "NATIVE_UCHAR";
    else if ( dt == PredType::NATIVE_SHORT ) return "NATIVE_SHORT";
    else if ( dt == PredType::NATIVE_USHORT ) return "NATIVE_USHORT";
    else if ( dt == PredType::NATIVE_UINT ) return "NATIVE_UINT";
    else if ( dt == PredType::NATIVE_LONG ) return "NATIVE_LONG";
    else if ( dt == PredType::NATIVE_ULONG ) return "NATIVE_ULONG";
    else if ( dt == PredType::NATIVE_LLONG ) return "NATIVE_LLONG";
    else if ( dt == PredType::NATIVE_ULLONG ) return "NATIVE_ULLONG";
    else if ( dt == PredType::NATIVE_DOUBLE ) return "NATIVE_DOUBLE";
    else if ( dt == PredType::NATIVE_LDOUBLE ) return "NATIVE_LDOUBLE";
    else if ( dt == PredType::NATIVE_B8 ) return "NATIVE_B8";
    else if ( dt == PredType::NATIVE_B16 ) return "NATIVE_B16";
    else if ( dt == PredType::NATIVE_B32 ) return "NATIVE_B32";
    else if ( dt == PredType::NATIVE_B64 ) return "NATIVE_B64";
    else if ( dt == PredType::NATIVE_OPAQUE ) return "NATIVE_OPAQUE";
    else if ( dt == PredType::NATIVE_HSIZE ) return "NATIVE_HSIZE";
    else if ( dt == PredType::NATIVE_HSSIZE ) return "NATIVE_HSSIZE";
    else if ( dt == PredType::NATIVE_HERR ) return "NATIVE_HERR";
    else if ( dt == PredType::NATIVE_HBOOL ) return "NATIVE_HBOOL";
    else if ( dt == PredType::NATIVE_INT8 ) return "NATIVE_INT8";
    else if ( dt == PredType::NATIVE_UINT8 ) return "NATIVE_UINT8";
    else if ( dt == PredType::NATIVE_INT16 ) return "NATIVE_INT16";
    else if ( dt == PredType::NATIVE_UINT16 ) return "NATIVE_UINT16";
    else if ( dt == PredType::NATIVE_INT32 ) return "NATIVE_INT32";
    else if ( dt == PredType::NATIVE_UINT32 ) return "NATIVE_UINT32";
    else if ( dt == PredType::NATIVE_INT64 ) return "NATIVE_INT64";
    else if ( dt == PredType::NATIVE_UINT64 ) return "NATIVE_UINT64";
    return "unknown";
}