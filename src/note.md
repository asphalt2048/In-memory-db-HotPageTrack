# Notes
## use typedef?
In kernel, programmer uses typedef for almost every struct:
```C
typedef struct slab_s{
    ...
}slab_t;
```
This is not recommended in user space programming with C/C++. 
In standard C, In standard C, if you define `struct slab_s { ... };`, you are forced to use the `struct` keyword every time you declare a variable. Kernel programmers use those typedef so that each time they declare a variable of such type, they don't have to type `struct`. 
In C++, a struct or union or others automatically inject their names into the namespace as fully-fledged types. We are safe to type `slab_s slab_64` directly.
What is more, '_t' suffix is actually reserved by POSIX. For kernel, where code is isolated from user-space library, using '_t' everywhere is safe. For user space code, if not managed carefully, this might cause some problem.

## static constexpr
Everybody knows, a static member belongs to the type but not to the object. constexper means the value of the variable is evalated completely at compile time. For a static constexpr variable, compiler might just hardcode its value to all the assem that use it.

## Arena's fragmentation?
Do we implement bins?

## uintptr_t
This is an unsigned integer type that is designed for pointers. This type is particularly useful for scenarios where pointers need to be manipulated or cast into integers for operations like arithmetic or bitwise manipulation.
- In 64 bits systems, uintptr_t is an alias for `unsigned long int`.
- In 32 bits systems, uintptr_t is an alias for `unsigned int`.

## hotness array in page header, not in record header

## pragma pack
#pragma pack is a compiler directive used to control the memory alignment of members within structures, unions, and classes. While proper alignment improves memory access efficiency, you may occasionally need a more compact layout to save space or satisfy specific hardware requirements.

### pragma pack(1) vs. #pragma pack(push, 1)
While both commands set the alignment to 1 byte, they differ in how they manage the compiler's state.

1. #pragma pack(1)
This command sets the alignment to 1 byte, meaning all member variables will be aligned consecutively without any padding bytes. This is particularly useful when handling network protocols or binary file formats, as it ensures the structure layout matches the external data format exactly.

```C++
#pragma pack(1)
struct TCPHEADER
{
    short SrcPort; // 16-bit source port
    short DstPort; // 16-bit destination port
    // ... other member variables
};
#pragma pack() // Resets to default alignment
```

2. #pragma pack(push, 1)
This version is more robust. It not only sets the current alignment to 1 byte but also pushes (saves) the previous alignment setting onto an internal stack. By using #pragma pack(pop) later, you can restore the original alignment.

This is ideal for temporary changes, as it allows you to apply a specific alignment to a single structure without permanently altering the global alignment settings for the rest of your code.

```C++
#pragma pack(push, 1) // Save current alignment and set to 1-byte alignment
struct MyStruct
{
    char a;
    int b;
    // ... other member variables
};
#pragma pack(pop) // Restore the previous alignment setting
```

## swap and exchange
Does `vector<RecordLoc>` automatically implements swap & exchange?

## pointer cal
You can't do void* + any type. 

## template
SC<1> and SC<2> is of different type. You can't put them in the same vector or array.

## CV
CV requires the mutex to be hold before wait() and signal() is called. Why? To protect that: during the time gap of checking the variable(condition) and calling wait, the condition haven't been changed.

## std::unique_lock, std::lock_guard

## slotted page used by B+ tree DB

## std::memory_order_relaxed?

## The Rule of Concurrency
Never publish an object to a globally visible data structure (LRU, Partial List, Hashmap) until it is in a 100% valid, secure state. A page is not secure until the creating thread has claimed at least one slot on it.