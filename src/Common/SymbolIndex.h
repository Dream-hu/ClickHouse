#pragma once

#if defined(__ELF__) && !defined(OS_FREEBSD)

#include <vector>
#include <string>
#include <Common/Elf.h>
#include <boost/noncopyable.hpp>


namespace DB
{

/** Allow to quickly find symbol name from address.
  * Used as a replacement for "dladdr" function which is extremely slow.
  * It works better than "dladdr" because it also allows to search private symbols, that are not participated in shared linking.
  */
class SymbolIndex : private boost::noncopyable
{
protected:
    SymbolIndex() { load(); }

public:
    static const SymbolIndex & instance();

    struct Symbol
    {
        const void * address_begin;
        const void * address_end;
        const char * name;
    };

    struct Object
    {
        const void * address_begin;
        const void * address_end;
        std::string name;
        std::shared_ptr<Elf> elf;
    };

    /// Address in virtual memory should be passed. These addresses include offset where the object is loaded in memory.
    const Symbol * findSymbol(const void * address) const;
    const Object * findObject(const void * address) const;

    const std::vector<Symbol> & symbols() const { return data.symbols; }
    const std::vector<Object> & objects() const { return data.objects; }

    /// The BuildID that is generated by compiler.
    String getBuildID() const { return data.build_id; }
    String getBuildIDHex() const;

    struct Data
    {
        std::vector<Symbol> symbols;
        std::vector<Object> objects;
        String build_id;
    };
private:
    Data data;

    void load();
};

}

#endif
