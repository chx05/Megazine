Store to disk and manage any C in-language structure in the most efficient way possible (for minimizing disk footprint and maximizing performance) without writing a single line of code or having to bother with sql-like dbs.

From the "magazine" concept of storing objects, to the "megabytes" idea of handling huge data seamlesly.

Serialization process:
* creates a dynamic fb (file buffer) (will be written entirely into the file)
* writes the ers (expanded root struct) to fb
* for each block field in ers -> writes the whole block content to fb, and replaces the pointer on the ers with the proper fb-offset, and saves the block as a fat pointer into a temp list of fb's blocks
* for each ref field in ers -> replaces that pointer with the proper fb-offset. emits a debug check in a post-fb-building function to see if the ref points to a block in the tmp fb-blocks list, if not error
* for each seq field in ers -> does the block logic, and calls the serialization process over each element of the sequence.


<br>

---

<br>

<p align="center">developed <code>@</code> ancient_labs</p>