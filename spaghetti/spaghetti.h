#ifndef SPAGHETTI_H_
#define SPAGHETTI_H_

#include <typeinfo>
#include <functional>
#include <stdexcept>
#include <cassert>
#include <vector>
#include <cxxabi.h>
#include <lua.hpp>
#include "cube.h"

namespace spaghetti{


extern lua_State* L;
extern bool quit;


void init();
void bindengine();
void bindserver();
void fini(bool error);


template<typename F, typename Err>
void lua_cppcall(const F& f, const Err& err){
    //XXX gcc bug, cannot use auto and decltype
    std::function<void()> environment = [&f](){
        try{ f(); return; }
        catch(const std::exception& e){
            const char* mangled = typeid(e).name();
            char* demangled;
            int status;
            demangled = abi::__cxa_demangle(mangled, 0, 0, &status);
            lua_pushfstring(L, "exception %s: %s", demangled ? demangled : mangled, e.what());
            free(demangled);
        }
        catch(...){
            lua_pushstring(L, "C++ exception (not a std::exception)");
        }
        lua_error(L);
    };
    extern int stackdumperref;
    lua_rawgeti(L, LUA_REGISTRYINDEX, stackdumperref);
    int dumperpos = lua_gettop(L);
    lua_pushcfunction(L, [](lua_State* L){
        (*(std::function<void()>*)lua_touserdata(L, 1))();
        return 0;
    });
    lua_pushlightuserdata(L, &environment);
    int result = lua_pcall(L, 1, 0, dumperpos);
    lua_remove(L, dumperpos);
    if(result == LUA_OK) return;
    std::string e = lua_tostring(L, -1);
    lua_pop(L, 1);
    err(e);
}

template<typename F>
void lua_cppcall(const F& f, bool thrw = true){
    if(thrw) lua_cppcall(f, [](std::string& err){ throw std::runtime_error(err); });
    else lua_cppcall(f, [](std::string&){});
}

inline std::function<void(std::string&)> cppcalldump(const char* fmt){
    return [fmt](std::string& err){ conoutf(CON_ERROR, fmt, err.c_str()); };
}


/*
 * Avoid creating a new string (malloc, hashing, etc) every time these are used.
 * If you add one here don't forget to initialize it in spaghetti.cpp!
 */
struct hotstring{
    enum{
        __index = 0, __newindex, __metatable, hooks, pf, skip, tick, shuttingdown, maxhotstring
    };
    static void push(int str){
        extern hotstring hotstringref[hotstring::maxhotstring];
        lua_rawgeti(L, LUA_REGISTRYINDEX, hotstringref[str]._ref);
    }
    static const char* get(int str){
        extern hotstring hotstringref[hotstring::maxhotstring];
        return hotstringref[str].literal;
    }
    static int ref(int str){
        extern hotstring hotstringref[hotstring::maxhotstring];
        return hotstringref[str]._ref;
    }
private:
    friend void init();
    const char* literal = 0;
    int _ref = LUA_NOREF;
};

inline void pushargs(){}
template<typename Arg, typename... Rest>
void pushargs(Arg&& arg, Rest&&... rest){
    luabridge::push(L, std::forward<Arg>(arg));
    pushargs(std::forward<Rest>(rest)...);
}

template<typename... Args>
void callhook(int name, Args&&... args){
    //XXX gcc doesn't support variadic captures. Need an extra indirection: http://stackoverflow.com/a/17667880/1073006
    auto pusher = std::bind([](Args&&... args){ pushargs(std::forward<Args>(args)...); }, std::forward<Args>(args)...);
    lua_cppcall([name,&pusher]{
        lua_pushglobaltable(L);
        hotstring::push(hotstring::hooks);
        lua_gettable(L, -2);
        hotstring::push(name);
        lua_gettable(L, -2);
        if(lua_type(L, -1) == LUA_TNIL) return;
        pusher();
        lua_call(L, sizeof...(Args), 0);
    }, cppcalldump((std::string("Error calling hook ") + hotstring::get(name) + ": %s").c_str()));
}


struct extra{
    void init(){
        lua_cppcall([this]{
            lua_newtable(L);
            this->ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }, cppcalldump("Cannot create extra table: %s"));
    }
    void fini(){
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
    }
private:
    friend struct ::luabridge::Stack<extra>;
    int ref = LUA_NOREF;
};


struct packetfilter{

    template<typename Field> packetfilter& operator()(const char* name, Field& field, int nameref = LUA_NOREF){
        addfield(name, field, nameref);
        return *this;
    }

    static bool testinterest(int type);
    static void object();

    /*
     * Utility function to be used with the skippacket(type, fields...) macro.
     * Also it is a template on how to use packetfilter (just ignore the literal parsing stuff).
     */
    template<int type, typename... Fields>
    static bool defaultfilter(const char* literal, Fields&... fields){
        static bool initialized = false;
        static std::vector<fieldname> names;
        if(!initialized){
            parsestringliteral(literal, names);
            assert(names.size() == sizeof...(fields));
            initialized = true;
        }
        //XXX cfr callhook
        auto fieldspusher = std::bind([](Fields&... fields){ addfield(names.begin(), fields...); }, fields...);
        bool skip = false;
        lua_cppcall([&]{
            if(!testinterest(type)) return;
            object();
            fieldspusher();
            addfield(0, skip, hotstring::ref(hotstring::skip));
            lua_call(L, 1, 0);
        }, [](std::string& err){ conoutf(CON_ERROR, "Error calling pf[%d]: %s", type, err.c_str()); });
        return skip;
    }

private:

    struct fieldname{
        std::string name;
        int ref = LUA_NOREF;
        fieldname(std::string&& name, int ref): name(std::move(name)), ref(ref){}
    };

    static void parsestringliteral(const char* literal, std::vector<fieldname>& names);

    template<typename T> static void addfield(const char* name, T& field, int nameref);

    static void addfield(std::vector<fieldname>::iterator){}
    template<typename Field, typename... Rest>
    static void addfield(std::vector<fieldname>::iterator names, Field& field, Rest&... rest){
        addfield(0, field, names->ref);
        addfield(++names, rest...);
    }

};

#define skippacket(type, ...) packetfilter::defaultfilter<type>(#__VA_ARGS__, ##__VA_ARGS__)


/*
 * Binary buffer helper. Makes lua able to read/write a buffer with strings, and resize it by setting the data length.
 * Ignore the blurb that comes next. Just use this boilerplate to bind a buffer (e.g., ENetBuffer):
 *
 * auto ebuff = lua_buff_table(&ENetBuffer::data, &ENetBuffer::dataLength);
 * ...
 * .beginClass<ENetBuffer>("ENetBuffer")
 *   .addProperty("dataLength", &ebuff.getLength, &ebuff.setLength)
 *   .addProperty("data", &ebuff.getBuffer, &ebuff.setBuffer)
 * .endClass()
 *
 */
template<typename S, typename B, typename L, B* S::*buffer, L S::*length, bool canresize=false, bool userealloc=true>
struct lua_buff{
    static size_t getLength(const S* s){
        return size_t(s->*length);
    }
    static std::string getBuffer(const S* s){
        return std::string((const char*)(s->*buffer), getLength(s));
    }
    static void setLength(S* s, size_t newlen){
        if(getLength(s) == newlen) return;
        constexpr bool isvoid = std::is_void<void>::value;
        using actualtype = typename std::conditional<isvoid, char, B>::type;
        s->*length = L(newlen);
        if(userealloc) s->*buffer = (B*)realloc(s->*buffer, newlen);
        else{
            static_assert(!(isvoid && !userealloc), "Cannot use new operator with void!");
            //cast needed to avoid compiler warning
            delete[] (actualtype*)(s->*buffer);
            s->*buffer = new actualtype[newlen];
        }
    }
    static void setBuffer(S* s, std::string newbuffer){
        if(canresize) setLength(s, newbuffer.size());
        memcpy(s->*buffer, newbuffer.c_str(), min(getLength(s), newbuffer.size()));
    }
};
//deduction
template<typename S, typename T> constexpr S mem_class(T S::*);
template<typename S, typename T> constexpr T mem_field(T S::*);
//omg
template<typename Buffer, Buffer buffer, typename Length, Length length, bool canresize=false, bool userealloc=true>
constexpr auto lua_buff_table()
-> lua_buff<decltype(mem_class(buffer)), typename std::remove_pointer<decltype(mem_field(buffer))>::type, decltype(mem_field(length)), buffer, length, canresize, userealloc>
{ return {}; }

#define lua_buff_table(buff, length, ...) lua_buff_table<decltype(buff), buff, decltype(length), length, ##__VA_ARGS__>()

}

namespace luabridge{

template<> struct Stack<spaghetti::extra>{
    static void push(lua_State* L, spaghetti::extra e){
        lua_rawgeti(L, LUA_REGISTRYINDEX, e.ref);
    }
    static spaghetti::extra get(lua_State* L, int index){
        return spaghetti::extra();
    }
};

}

#endif /* SPAGHETTI_H_ */
