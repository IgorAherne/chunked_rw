# chunked_rw

Chunked Reader Writer, for efficient processing of chunk, while another chunk is being loaded from Hard Drive 
(or while it's being written, in the case of Writer).

<b>file_reader_chunks:</b>
When using the reader, you just need to invoke a method for getting the next literal. 
The reader automatically begins fetching the next chunk asynchronously, when it's time to get more stuff.

<b>file_writer_chunks:</b>
Writer beahves similarly. You provide it raw bytes, and it will automatically save the data to the file, once you've given it a sufficient amount.
This chunk will be saved asynchronously to the file, while you are providing further data.
