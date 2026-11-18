# Language highlighting example

Toggle Edit/Read with Cmd+Shift+M on Mac (Ctrl+Shift+M elsewhere). Code keeps the writing font and zoom; tokens use the current light/dark syntax palette.

## Python: keywords, strings, numbers and comments

```python
def greet(name):
    """A multiline string.
    State continues onto this line.
    """
    return f"Hello, {name} 🌍"  # UTF-16 offsets stay correct.

answer = 42
```

## C++

```cpp
#include <string>
/* A multiline comment.
   This remains a comment. */
const std::string greeting = "hello";
int answer = 42;
```

## JavaScript / TypeScript

```js
const enabled = true;
console.log("hello"); // comment
```

```ts
interface Note { title: string; }
const note: Note = { title: "Markdown" };
```

## JSON and YAML

```json
{"enabled": true, "count": 12, "name": "sample"}
```

```yaml
enabled: true
count: 12 # comment
```

## Shell alias

```sh
name="writer"
printf '%s\n' "$name" # comment
```

## Separate fences reset lexer state

The deliberately unfinished string below must not turn the next fence into a string:

```python
'''unfinished string
```
```python
return 123
```

## Plain fallback

An unknown language or an untagged fence stays plain code. Markdown emphasis markers inside code stay literal.

```unknown-omawrite-language
plain _code_ with **literal markers**
```

```
untagged_var = 99
```
