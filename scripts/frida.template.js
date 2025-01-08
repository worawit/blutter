const ShowNullField = false;
const MaxDepth = 5;
var libapp = null;

function onLibappLoaded() {
    xxx("remove this line and correct the hook value");
    const fn_addr = 0xdeadbeef;
    Interceptor.attach(libapp.add(fn_addr), {
        onEnter: function () {
            init(this.context);
            let objPtr = getArg(this.context, 0);
            const [tptr, cls, values] = getTaggedObjectValue(objPtr);
            console.log(`${cls.name}@${tptr.toString().slice(2)} =`, JSON.stringify(values, null, 2));
        }
    });
}

function tryLoadLibapp() {
    libapp = Module.findBaseAddress('libapp.so');
    if (libapp === null)
        setTimeout(tryLoadLibapp, 500);    
    else
        onLibappLoaded();
}
tryLoadLibapp();

const PointerCompressedEnabled = true;
const CompressedWordSize = 4;
const HeapAddressReg = 'x28';
const NullReg = 'x22';
const StackReg = 'x15';

if (!PointerCompressedEnabled)
    console.error("now support only compressed pointer");

let HeapAddress = 0;
// this function must be called at least on first interception of Dart function
function init(context) {
    if (HeapAddress === 0) {
        // heap bit register value is not shifted
        HeapAddress = context[HeapAddressReg].shl(32);
    }
}

function getDartBool(ptr, cls) {
    return ptr.add(cls.valOffset).readU8() != 0;
}

function getDartMint(ptr, cls) {
    return ptr.add(cls.valOffset).readU64();
}

function getDartDouble(ptr, cls) {
    return ptr.add(cls.valOffset).readDouble();
}

function getDartString(ptr, cls) {
    const len = ptr.add(cls.lenOffset).readU32() >> 1; // Dart store string length as Smi
    return ptr.add(cls.dataOffset).readUtf8String(len);
}

function getDartArray(ptr, cls, depthLeft, glen = null) {
    // TODO: type arguments
    // Dart store array length as Smi
    const len = glen === null ? ptr.add(cls.lenOffset).readU32() >> 1 : glen;
    let vals = [];
    let dataPtr = ptr.add(cls.dataOffset);
    for (let i = 0; i < len; i++) {
        let dptr = dataPtr.add(i * CompressedWordSize).readPointer();
        const [tptr, ocls, fieldValue] = getTaggedObjectValue(dptr, depthLeft - 1);
        if ([CidNull, CidSmi, CidMint, CidDouble, CidBool, CidString].includes(ocls.id)) {
            vals.push(fieldValue);
        }
        else {
            const key = `${ocls.name}@${tptr.toString().slice(2)}`;
            vals.push({key: fieldValue});
        }
    }
    return vals;
}

function getDartGrowableArray(ptr, cls, depthLeft) {
    // TODO: type arguments
    const len = ptr.add(cls.lenOffset).readU32() >> 1; // Dart store array length as Smi
    let arrPtr = ptr.add(cls.dataOffset);
    return getDartArray(arrPtr, Classes[CidArray], depthLeft, len);
}

function getDartTypedArrayValues(ptr, cls, elementSize, readValFn) {
    const len = ptr.add(cls.lenOffset).readU32() >> 1;
    let dataPtr = ptr.add(cls.dataOffset);
    let vals = [];
    for (let i = 0; i < len; i++) {
        let val = readValFn(dataPtr.add(i * elementSize));
        vals.push(val);
    }
    return vals;
}

function isFieldNative(fieldBitmap, offset) {
    const idx = offset / CompressedWordSize;
    return (fieldBitmap & (1 << idx)) !== 0;
}

// tptr (tagged pointer) is only for tagged object (HeapBit in address except Smi)
// return format: value
function getObjectValue(ptr, cls, depthLeft = MaxDepth) {
    switch (cls.id) {
    case CidObject:
        console.error(`Object cid should not reach here`);
        return;
    case CidNull:
        return null;
    case CidBool:
        return getDartBool(ptr, cls);
    case CidString:
        return getDartString(ptr, cls);
    case CidMint:
        return getDartMint(ptr, cls);
    case CidDouble:
        return getDartDouble(ptr, cls);
    case CidArray:
        return getDartArray(ptr, cls, depthLeft);
    case CidGrowableArray:
        return getDartGrowableArray(ptr, cls, depthLeft);
    case CidUint8Array:
        return getDartTypedArrayValues(ptr, cls, 1, (p) => p.readU8());
    case CidInt8Array:
        return getDartTypedArrayValues(ptr, cls, 1, (p) => p.readS8());
    case CidUint16Array:
        return getDartTypedArrayValues(ptr, cls, 2, (p) => p.readU16());
    case CidInt16Array:
        return getDartTypedArrayValues(ptr, cls, 2, (p) => p.readS16());
    case CidUint32Array:
        return getDartTypedArrayValues(ptr, cls, 4, (p) => p.readU32());
    case CidInt32Array:
        return getDartTypedArrayValues(ptr, cls, 4, (p) => p.readS32());
    case CidUint64Array:
        return getDartTypedArrayValues(ptr, cls, 8, (p) => p.readU64());
    case CidInt64Array:
        return getDartTypedArrayValues(ptr, cls, 8, (p) => p.readS64());
    // _TwoByteString
    case 94: {
        const { lenOffset = 8, dataOffset = 16 } = cls
        const len = ptr.add(lenOffset).readU32() >> 1;
        return ptr.add(dataOffset).readUtf16String(len); 
    }}

    if (cls.id < NumPredefinedCids) {
        const msg = `Unhandle class id: ${cls.id}, ${cls.name}`;
        console.log(msg);
        return msg;
    }
    
    if (depthLeft <= 0) {
        return 'no more recursive';
    }

    // find parent tree
    let parents = [];
    let scls = Classes[cls.sid];
    while (scls.id != CidObject) {
        parents.push(scls);
        scls = Classes[scls.sid];
    }
    // get value from top parent to bottom parent
    let values = {};
    while (parents.length > 0) {
        const sscls = scls;
        scls = parents.pop();
        const parentValue = getInstanceValue(ptr, scls, sscls, depthLeft);
        values[`parent!${scls.name}`] = parentValue;
    }
    const myValue = getInstanceValue(ptr, cls, scls, depthLeft);
    Object.assign(values, myValue);
    return values;
}

function getInstanceValue(ptr, cls, scls, depthLeft = MaxDepth) {
    let values = {};
    let offset = scls.size;
    while (offset < cls.size) {
        if (offset == cls.argOffset) {
            // TODO: type arguments
            offset += CompressedWordSize;
        }
        else if (isFieldNative(cls.fbitmap, offset)) {
            if (PointerCompressedEnabled && !isFieldNative(cls.fbitmap, offset + CompressedWordSize))
                console.error("Native type but use only 4 bytes");
            
            let val = ptr.add(offset).readU64();
            // TODO: this might not work on javascript because all number are floating pointer (except BigInt)
            // it is rare to find integer that larger than 0x1000_0000_0000_0000
            //   while double is very common because of exponent value
            // to know exact type (int or double), we have to check from register type in assembly
            if (val <= 0x1000000000000000n || val >= 0xffffffffffff0000n) {
                // it should be integer
                values[`off_${offset.toString(16)}`] = val;
            }
            else {
                val = ptr.add(offset).readDouble();
                values[`off_${offset.toString(16)}`] = val;
            }
            offset += CompressedWordSize * 2;
        }
        else {
            // object
            let dptr = ptr.add(offset).readPointer();
            const [tptr, ocls, fieldValue] = getTaggedObjectValue(dptr, depthLeft - 1);
            if (ocls.id === CidSmi) {
                values[`off_${offset.toString(16)}!Smi`] = fieldValue;
            }
            else if (ocls.id !== CidNull) {
                values[`off_${offset.toString(16)}!${ocls.name}@${tptr.toString().slice(2)}`] = fieldValue;
            }
            else if (ShowNullField) {
                values[`off_${offset.toString(16)}`] = fieldValue;
            }
            offset += CompressedWordSize;
        }
    }

    return values;
}

// tptr (tagged pointer) is only for tagged object (HeapBit in address except Smi)
// return format: [tptr, cls, values]
function getTaggedObjectValue(tptr, depthLeft = MaxDepth) {
    if (!isHeapObject(tptr)) {
        // smi
        // TODO: below support only compressed pointer (4 bytes)
        return [tptr, Classes[CidSmi], tptr.toInt32() >> 1];
    }

    tptr = decompressPointer(tptr);
    let ptr = tptr.sub(1);
    const cls = Classes[getObjectCid(ptr)];
    const values = getObjectValue(ptr, cls, depthLeft);
    return [tptr, cls, values];
}

function getArg(context, idx) {
    // Note: argument pointer is never compressed
    let stack = context[StackReg];
    return stack.add(8 * idx).readPointer();
}

function isHeapObject(ptr) {
    return (ptr.toInt32() & 1) == 1;
}

function getObjectTag(ptr) {
    const tag = ptr.readU64();
    const objSize = ((tag >> 8) & 0xf) * 8;
    const cid = (tag >> ClassIdTagPos) & ClassIdTagMask;
    //const hashId = (tag >> 32) & 0xffffffff;
    return [cid, objSize];
}

function getObjectCid(ptr) {
    const tag = ptr.readU32();
    return (tag >> ClassIdTagPos) & ClassIdTagMask;
}

function decompressPointer(dptr) {
    if (PointerCompressedEnabled) {
        if (HeapAddress === 0)
            console.error("Uninitialized HeapAddress");
        return HeapAddress.add(dptr.toInt32());
    }
    return dptr;
}
