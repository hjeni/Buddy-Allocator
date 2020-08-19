# Buddy-Allocator
Simplified memory allocation API using buddy allocator &amp; bitmap techniques

This is a school project from BI-OSY at FIT CTU. 

The solution implements simple API which can be used for memory allocation. The API consists of 4 functions: 
* <b>HeapInit()</b> - Initializes new heap with a given memory block
* <b>HeapAlloc()</b> - Allocates memory block of given size on the heap
* <b>HeapFree()</b> - Frees memory block specified by a pointer
* <b>HeapDone()</b> - Returns number of unfreed blocks in the heap

The solution is not encapsulated in any OOP structure as it was required in the project assignment.
