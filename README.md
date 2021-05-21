# chunked_rw

"Chunked" Reader and Writer, to allow you to process already loaded bytes, while it loads more from Hard Drive asynchronously
</br>(or while it writes, in the case of Writer). </br>
Groups of bytes are reffered to as "Chunk". Saving such groups of bytes all-at-once, in large groups, will saturate bandidth. </br>
It will help to minimise the latency, which is important because fetching from Disk is a lot slower than fetchign from RAM.
</br>
</br>
</br>
<b>file_reader_chunks:</b></br></br>
When using the reader, you just need to invoke a method for getting a next literal.</br> 
The reader automatically begins fetching the next chunk asynchronously, when it's time to get more stuff.

<b>file_writer_chunks:</b></br></br>
Writer beahves similarly. Provide it raw bytes, and it will automatically save the data to the file, once you've given it a sufficient amount.</br>
Such a chunk will be saved asynchronously to the file, while you are providing some further data.
