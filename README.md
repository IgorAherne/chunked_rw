# chunked_rw

"Chunked" Reader and Writer, for efficient processing of already loaded bytes, while more is still being loaded from Hard Drive 
(or while it's being written, in the case of Writer). 
Groups of bytes are reffered to as "Chunk"
</br>
</br>
</br>
<b>file_reader_chunks:</b></br></br>
When using the reader, you just need to invoke a method for getting the next literal. 
The reader automatically begins fetching the next chunk asynchronously, when it's time to get more stuff.

<b>file_writer_chunks:</b></br></br>
Writer beahves similarly. You provide it raw bytes, and it will automatically save the data to the file, once you've given it a sufficient amount.
This chunk will be saved asynchronously to the file, while you are providing further data.
