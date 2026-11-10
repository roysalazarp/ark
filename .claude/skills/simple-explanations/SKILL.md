---
name: simple-explanations
description: Explain technical topics in very simple, short, easy-to-understand language for someone who is learning. Use this whenever the user asks what something means, how or why something works, what a piece of code does, or asks to explain a concept, struct, function, error or decision, even if they only say "why?", "what do you mean?" or "explain this". Also use it when summarizing work you just did.
---

# Simple explanations

The reader is learning. They want to understand the idea, not read everything
there is to know about it. A short explanation they fully understand beats a
complete one they skim.

## How to explain

1. **Answer first.** Put the answer in the first sentence or two. Details come after.
2. **Keep it short.** Aim for a few sentences, and about 100 words for most
   questions. Go longer only when the question needs it.
3. **Use plain words.** Prefer short, everyday words and short sentences.
4. **Define new terms.** When a technical word is needed, explain it in a few
   words the first time. Learners need these words, so keep them, but make them clear.
5. **One idea at a time.** Cover the main idea only. Leave out side topics,
   history and rare edge cases.
6. **Use one small example or comparison** when it makes the idea clearer: a few
   lines of code, or something from everyday life. Don't stack several.
7. **Keep formatting light.** Prefer plain sentences. Use a short list for real
   steps. Avoid tables and many headings, because they make answers feel long.
8. **Offer more only if useful.** One short line at the end is enough, such as
   "Want me to go deeper on X?"

## Stay correct

Simple must still be true. Leave details out, but don't say anything false to
make it easier. If a comparison breaks down in a way that matters, say so in one
short sentence. If leaving something out could cause a real mistake, keep that
one warning. If you are unsure, say so.

## Example

Question: "Why does the server wait at most 1 second?"

Too much: a long answer with sections on timeouts, hot reload, trade-offs
between 10 ms and 10 s, and future designs.

Good:

> So it can wake up even when nobody is sending anything.
>
> The server closes connections that stay quiet for too long. To notice that,
> it has to be awake. If it slept until the next message, a quiet connection
> might never get closed. Waking once a second is enough, and costs almost nothing.

## When this does not apply

Keep code, commit messages, and documents written for other people in the style
they need. This skill is about how you explain things to the user.
