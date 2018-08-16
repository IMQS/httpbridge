2017-05-18 InFrame.BodyBytes when buffered
==========================================

I just uncovered a bug where InFrame::Reset() was trying to free BodyBytes, because it
saw that BodyBytes was not inside Request->BodyBuffer, and therefore it believes that
the buffer belonged to the InFrame object. This was not true, because Request->BodyBuffer
starts out small, and grows bigger with incoming frames. InFrame assumed that the
entire buffer was allocated up-front, and so the pointer would never change. This was
the root cause of the bug.

Upon further reflection, I've realized that it's a thread hazard to try and maintain
a sensible InFrame::BodyBytes when the request is being buffered, because you might
be processing that InFrame on one of your HTTP threads, while the main httpbridge
thread is busy adding new data to the buffer. In other words, if we try and be clever,
and make InFrame::BodyBytes point inside Request->BodyBuffer, then BodyBytes is likely
to be stale, as BodyBuffer grows.

So, what I'm doing instead, is to always make BodyBytes null, if the request is
being buffered. The same goes for BodyBytesLen: it is set to zero if the request is 
being buffered. I feel that there is little value in being able to inspect incoming
frames, while you're actually waiting for the entire request to be buffered. If you're
really interested in accepting incremental uploads, then you're probably going to be
handling that yourself, and not asking httpbridge to buffer the requests.

