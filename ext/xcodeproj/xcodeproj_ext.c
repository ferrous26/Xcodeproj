// TODO
// * free memory when raising

#include "extconf.h"

#include "ruby.h"
#if HAVE_RUBY_ST_H
#include "ruby/st.h"
#elif HAVE_ST_H
#include "st.h"
#endif

#include "CoreFoundation/CoreFoundation.h"
#include "CoreFoundation/CFStream.h"
#include "CoreFoundation/CFPropertyList.h"

VALUE Xcodeproj = Qnil;


#define CONVERT_CFNUM(type, cookie, macro) do {		        \
    type value;							\
    if (CFNumberGetValue(num, cookie, &value))			\
      return macro(value);					\
    rb_raise(rb_eRuntimeError, "I goofed wrapping a number");	\
    return Qnil;						\
  } while(0);

#define CONVERT_NUM(type, cookie, macro) do {	\
    type base = macro(num);			\
    return CFNumberCreate(NULL, cookie, &base); \
  } while(0);

static VALUE convert_cflong(CFNumberRef num)      { CONVERT_CFNUM(long, kCFNumberLongType, LONG2FIX);          }
static VALUE convert_cflong_long(CFNumberRef num) { CONVERT_CFNUM(long long, kCFNumberLongLongType, LL2NUM);   }
static VALUE convert_cffloat(CFNumberRef num)     { CONVERT_CFNUM(double,    kCFNumberDoubleType,   rb_float_new);  }

static CFNumberRef convert_rbfixnum(VALUE num) { CONVERT_NUM(long,      kCFNumberLongType,     NUM2LONG); }
static CFNumberRef convert_rbbignum(VALUE num) { CONVERT_NUM(long long, kCFNumberLongLongType, NUM2LL);   }
static CFNumberRef convert_rbfloat(VALUE num)  { CONVERT_NUM(double,    kCFNumberDoubleType,   NUM2DBL);  }

static VALUE
cfnum_to_num(CFNumberRef number) {
  switch (CFNumberGetType(number)) {
  case kCFNumberSInt8Type:
  case kCFNumberSInt16Type:
  case kCFNumberSInt32Type:
  case kCFNumberSInt64Type:
    return convert_cflong(number);
  case kCFNumberFloat32Type:
  case kCFNumberFloat64Type:
    return convert_cffloat(number);
  case kCFNumberCharType:
  case kCFNumberShortType:
  case kCFNumberIntType:
  case kCFNumberLongType:
    return convert_cflong(number);
  case kCFNumberLongLongType:
    return convert_cflong_long(number);
  case kCFNumberFloatType:
  case kCFNumberDoubleType:
    return convert_cffloat(number);
  case kCFNumberCFIndexType:
  case kCFNumberNSIntegerType:
    return convert_cflong(number);
  case kCFNumberCGFloatType: // == kCFNumberMaxType
    return convert_cffloat(number);
  default:
    return INT2NUM(0); // unreachable unless system goofed
  }
}

static CFNumberRef
num_to_cfnum(VALUE number) {
  switch (TYPE(number)) {
  case T_FIXNUM:
    return convert_rbfixnum(number);
  case T_FLOAT:
    return convert_rbfloat(number);
  case T_BIGNUM:
    return convert_rbbignum(number);
  default:
    rb_raise(
	     rb_eRuntimeError,
	     "wrapping %s is not supported; log a bug?",
	     rb_string_value_cstr(&number)
	     );
    return kCFNumberNegativeInfinity; // unreachable
  }
}

static VALUE
cfstr_to_str(const void *cfstr) {
  CFDataRef data = CFStringCreateExternalRepresentation(NULL, cfstr, kCFStringEncodingUTF8, 0);
  assert(data != NULL);
  long  len = (long)CFDataGetLength(data);
  char *buf = (char *)CFDataGetBytePtr(data);

  register VALUE str = rb_str_new(buf, len);

  // force UTF-8 encoding in Ruby 1.9+
  ID forceEncodingId = rb_intern("force_encoding");
  if (rb_respond_to(str, forceEncodingId)) {
    rb_funcall(str, forceEncodingId, 1, rb_str_new("UTF-8", 5));
  }

  CFRelease(data);
  return str;
}

// Coerces to String as well.
static CFStringRef
str_to_cfstr(VALUE str) {
  return CFStringCreateWithCString(NULL, RSTRING_PTR(rb_String(str)), kCFStringEncodingUTF8);
}

/* Generates a UUID. The original version is truncated, so this is not 100%
 * guaranteed to be unique. However, the `PBXObject#generate_uuid` method
 * checks that the UUID does not exist yet, in the project, before using it.
 *
 * @note Meant for internal use only.
 *
 * @return [String] A 24 characters long UUID.
 */
static VALUE
generate_uuid(void) {
  CFUUIDRef uuid = CFUUIDCreate(NULL);
  CFStringRef strRef = CFUUIDCreateString(NULL, uuid);
  CFRelease(uuid);

  CFArrayRef components = CFStringCreateArrayBySeparatingStrings(NULL, strRef, CFSTR("-"));
  CFRelease(strRef);
  strRef = CFStringCreateByCombiningStrings(NULL, components, CFSTR(""));
  CFRelease(components);

  UniChar buffer[24];
  CFStringGetCharacters(strRef, CFRangeMake(0, 24), buffer);
  CFStringRef strRef2 = CFStringCreateWithCharacters(NULL, buffer, 24);

  VALUE str = cfstr_to_str(strRef2);
  CFRelease(strRef);
  CFRelease(strRef2);
  return str;
}


static void
hash_set(const void *keyRef, const void *valueRef, void *hash) {
  VALUE key = cfstr_to_str(keyRef);
  register VALUE value = Qnil;

  CFTypeID valueType = CFGetTypeID(valueRef);
  if (valueType == CFStringGetTypeID()) {
    value = cfstr_to_str(valueRef);

  } else if (valueType == CFDictionaryGetTypeID()) {
    value = rb_hash_new();
    CFDictionaryApplyFunction(valueRef, hash_set, (void *)value);

  } else if (valueType == CFArrayGetTypeID()) {
    value = rb_ary_new();
    CFIndex i, count = CFArrayGetCount(valueRef);
    for (i = 0; i < count; i++) {
      CFTypeRef elementRef = CFArrayGetValueAtIndex(valueRef, i);
      CFTypeID elementType = CFGetTypeID(elementRef);
      if (elementType == CFStringGetTypeID()) {
        rb_ary_push(value, cfstr_to_str(elementRef));

      } else if (elementType == CFDictionaryGetTypeID()) {
        VALUE hashElement = rb_hash_new();
        CFDictionaryApplyFunction(elementRef, hash_set, (void *)hashElement);
        rb_ary_push(value, hashElement);

      } else {
        CFStringRef descriptionRef = CFCopyDescription(elementRef);
        // obviously not optimal, but we're raising here, so it doesn't really matter
        VALUE description = cfstr_to_str(descriptionRef);
        rb_raise(rb_eTypeError, "Plist array value contains a object type unsupported by Xcodeproj. In: `%s'", RSTRING_PTR(description));
        CFRelease(descriptionRef);
      }
    }

  } else if (valueType == CFNumberGetTypeID()) {
    value = cfnum_to_num(valueRef);

  } else if (valueType == CFBooleanGetTypeID()) {
    value = CFBooleanGetValue(valueRef) ? Qtrue : Qfalse;

  } else {
    rb_raise(rb_eTypeError, "Plist contains a hash value object type unsupported by Xcodeproj.");
  }

  rb_hash_aset((VALUE)hash, key, value);
}

static int
dictionary_set(st_data_t key, st_data_t value, CFMutableDictionaryRef dict) {
  CFStringRef keyRef = str_to_cfstr(key);
  long i, count;

  CFTypeRef valueRef = NULL;
  switch (TYPE(value)) {
  case T_HASH:
    valueRef = CFDictionaryCreateMutable(NULL,
                                         0,
                                         &kCFTypeDictionaryKeyCallBacks,
                                         &kCFTypeDictionaryValueCallBacks);
    rb_hash_foreach(value, dictionary_set, (st_data_t)valueRef);
    break;

  case T_ARRAY:
    count = RARRAY_LEN(value);
    valueRef = CFArrayCreateMutable(NULL, count, &kCFTypeArrayCallBacks);
    for (i = 0; i < count; i++) {
      VALUE element = RARRAY_PTR(value)[i];
      CFTypeRef elementRef = NULL;
      if (TYPE(element) == T_HASH) {
        elementRef = CFDictionaryCreateMutable(NULL,
                                               0,
                                               &kCFTypeDictionaryKeyCallBacks,
                                               &kCFTypeDictionaryValueCallBacks);
        rb_hash_foreach(element, dictionary_set, (st_data_t)elementRef);
      } else {
        // otherwise coerce to string
        elementRef = str_to_cfstr(element);
      }
      CFArrayAppendValue((CFMutableArrayRef)valueRef, elementRef);
      CFRelease(elementRef);
    }
    break;

  case T_FIXNUM:
  case T_BIGNUM:
  case T_FLOAT:
    valueRef = num_to_cfnum(value);
    break;

  case T_TRUE:
    valueRef = kCFBooleanTrue;
    break;

  case T_FALSE:
    valueRef = kCFBooleanFalse;
    break;

  default:
    valueRef = str_to_cfstr(value);
  }

  if (valueRef == NULL) {
    rb_raise(rb_eTypeError, "Unable to convert value of key `%s'.", RSTRING_PTR(rb_inspect(key)));
  }

  CFDictionaryAddValue(dict, keyRef, valueRef);
  CFRelease(keyRef);
  CFRelease(valueRef);
  return ST_CONTINUE;
}

static CFURLRef
str_to_url(VALUE path) {
#ifdef FilePathValue
  VALUE p = FilePathValue(path);
#else
  VALUE p = rb_String(path);
#endif
  CFURLRef fileURL = CFURLCreateFromFileSystemRepresentation(NULL, RSTRING_PTR(p), RSTRING_LEN(p), false);
  if (!fileURL) {
    rb_raise(rb_eArgError, "Unable to create CFURL from `%s'.", RSTRING_PTR(rb_inspect(path)));
  }
  return fileURL;
}


/* @overload read_plist(path)
 *
 * Reads from the specified path and de-serializes the property list.
 *
 * @note This does not yet support all possible types that can exist in a valid property list.
 *
 * @note This currently only assumes to be given an Xcode project document.
 *       This means that it only accepts dictionaries, arrays, and strings in
 *       the document.
 *
 * @param [String] path  The path to the property list file.
 * @return [Hash]        The dictionary contents of the document.
 */
static VALUE
read_plist(VALUE self, VALUE path) {
  CFPropertyListRef dict;
  CFStringRef       errorString;
  CFDataRef         resourceData;
  SInt32            errorCode;

  CFURLRef fileURL = str_to_url(path);
  if (CFURLCreateDataAndPropertiesFromResource(NULL, fileURL, &resourceData, NULL, NULL, &errorCode)) {
    CFRelease(fileURL);
  }
  if (!resourceData) {
    rb_raise(rb_eArgError, "Unable to read data from `%s'", RSTRING_PTR(rb_inspect(path)));
  }

  dict = CFPropertyListCreateFromXMLData(NULL, resourceData, kCFPropertyListImmutable, &errorString);
  if (!dict) {
    rb_raise(rb_eArgError, "Unable to read plist data from `%s': %s", RSTRING_PTR(rb_inspect(path)), RSTRING_PTR(cfstr_to_str(errorString)));
  }
  CFRelease(resourceData);

  register VALUE hash = rb_hash_new();
  CFDictionaryApplyFunction(dict, hash_set, (void *)hash);
  CFRelease(dict);

  return hash;
}

/* @overload write_plist(hash, path)
 *
 * Writes the serialized contents of a property list to the specified path.
 *
 * @note This does not yet support all possible types that can exist in a valid property list.
 *
 * @note This currently only assumes to be given an Xcode project document.
 *       This means that it only accepts dictionaries, arrays, and strings in
 *       the document.
 *
 * @param [Hash] hash     The property list to serialize.
 * @param [String] path   The path to the property list file.
 * @return [true, false]  Wether or not saving was successful.
 */
static VALUE
write_plist(VALUE self, VALUE hash, VALUE path) {
  VALUE h = rb_check_convert_type(hash, T_HASH, "Hash", "to_hash");
  if (NIL_P(h)) {
    rb_raise(rb_eTypeError, "%s can't be coerced to Hash", rb_obj_classname(hash));
  }

  CFMutableDictionaryRef dict = CFDictionaryCreateMutable(NULL,
                                                          0,
                                                          &kCFTypeDictionaryKeyCallBacks,
                                                          &kCFTypeDictionaryValueCallBacks);

  rb_hash_foreach(h, dictionary_set, (st_data_t)dict);

  CFURLRef fileURL = str_to_url(path);
  CFWriteStreamRef stream = CFWriteStreamCreateWithFile(NULL, fileURL);
  CFRelease(fileURL);

  CFIndex success = 0;
  if (CFWriteStreamOpen(stream)) {
    CFStringRef errorString;
    success = CFPropertyListWriteToStream(dict, stream, kCFPropertyListXMLFormat_v1_0, &errorString);
    if (!success) {
      CFShow(errorString);
    }
  } else {
    printf("Unable to open stream!\n");
  }

  CFRelease(dict);
  return success ? Qtrue : Qfalse;
}


void Init_xcodeproj_ext() {
  Xcodeproj = rb_define_module("Xcodeproj");
  rb_define_singleton_method(Xcodeproj, "generate_uuid", generate_uuid, 0);
  rb_define_singleton_method(Xcodeproj, "read_plist", read_plist, 1);
  rb_define_singleton_method(Xcodeproj, "write_plist", write_plist, 2);
}
