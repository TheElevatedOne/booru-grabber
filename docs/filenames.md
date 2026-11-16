# Filename format

The filename format decides where each downloaded image goes and what it is called. Set it in
**Tools → Settings → Saving → Filename**, with `-f` / `--filename` in `booru-grabber-cli`, or per download group.
It is a path relative to the download folder: every `/` starts a new subfolder.

Formats written for the original Grabber work unchanged.

```text
%artist%/%copyright%/%character%/%md5%.%ext%
```

```text
namu (ratias7777)/sword art online/leafa/362333f4fb4c458577ee72919cfbe10d.png
moneti (daifuku)/touhou/flandre scarlet/0498c78f67d5545bea731d4b1051096c.jpg
```

A format must end with `.%ext%` and contain `%md5%`, `%id%` or `%num%`, otherwise two images could get the same
name. The settings dialog shows a warning when it doesn't.

## Sections

- [Tokens](#tokens)
- [Token options](#token-options)
- [Several values in one token](#several-values-in-one-token)
- [Custom tokens](#custom-tokens)
- [Conditionals](#conditionals)
- [Conditional filenames](#conditional-filenames)
- [JavaScript](#javascript)

## Tokens

Write a token between percent signs: `%md5%`.

### Image

| Token | Value |
|---|---|
| `md5` | MD5 of the file. Computed after download when the site doesn't return it. |
| `id` | Image ID on its site |
| `ext` | File extension, without the dot |
| `filename` | Filename on the site's server, without extension |
| `rating` | `general`, `safe`, `questionable`, `explicit` or `unknown` |
| `score` | Score on the site |
| `width`, `height` | Size in pixels |
| `mpixels` | `width × height` |
| `ratio` | `width / height` |
| `filesize` | Size in bytes |
| `filetype` | Same as `ext` |
| `date` | Upload date (see [dates](#dates)) |
| `author`, `authorid` | Uploader name and ID |
| `parentid` | ID of the parent post, when the site returns it |
| `source` | Source URL of the artwork; `sources` lists all of them |
| `position` | Position in the results or the gallery, from 1 |
| `url_file`, `url_original`, `url_sample`, `url_thumbnail`, `url_page` | URLs of the file and its pages |
| `website` | Site host, e.g. `safebooru.org` |
| `websitename` | Site name as set in **Sites → Settings** |

### Tags

| Token | Value |
|---|---|
| `artist`, `copyright`, `character`, `general`, `model`, `species`, `meta`, `lore`, `contributor`, `photo_set` | Tags of that type. What happens when there are none or several is set per type, see [Several values](#several-values-in-one-token). |
| `all` | All tags |
| `tags` | All tags with their types (for JavaScript) |
| `allo` | All tags without any cleaning. Only for commands; it can produce names Linux can't save. |
| `search` | The search that found the image, as typed |
| `search_1`, `search_2`, … | First, second, … tag of the search |

### Download

| Token | Value |
|---|---|
| `count` | Counter of the current batch download, from 1 |
| `num` | Number that goes up when a file with the same name already exists: `file 1.jpg`, `file 2.jpg`, … `%num:noext%` also counts files with another extension |
| `current_date` | Time of the download |
| `gallery.id`, `gallery.name`, `gallery.<token>` | Tokens of the gallery the image belongs to |
| `grabber` | `alreadyExists`, `inMd5List`, `downloaded`, `favorited`, `monitored` for this image. Mostly for post-filters: `-grabber:downloaded` |
| `grabber_version` | Booru Grabber's version |

## Token options

Add options after a colon, separated by commas: `%token:option1=value,option2%`. An option without a value is
`true`. Escape a comma inside a value with `^`: `%general:separator=^,%`.

### Any token

| Option | Effect | Example | Result |
|---|---|---|---|
| `maxlength=N` | Cut the value to N characters | `%md5:maxlength=8%` | `1bc29b36` |
| `unsafe` | Keep `\ % / : \| * ? " < >` (by default they become `_`) | `%copyright:unsafe%` | `fate/stay_night` |
| `underscores` | Keep underscores even when tags are written with spaces | `%copyright:underscores%` | `fate_stay_night` |
| `spaces` | Write spaces even when **Keep underscores in tags** is on | `%copyright:spaces%` | `fate stay night` |
| `escape` | Quote and escape for SQL (commands) | `%all:escape%` | `'let''s_try'` |
| `htmlescape` | Escape `< > & "` for HTML | `%all:htmlescape%` | `&gt;_&lt;` |
| `raw` | No cleaning at all | `%source:raw%` | |

### Numbers

| Option | Effect | Example | Result |
|---|---|---|---|
| `length=N` | Pad with zeros to N digits | `%id:length=6%` | `007331` |
| `precision=N` | Digits after the decimal point | `%ratio:precision=2%` | `1.33` |

### Lists

| Option | Effect | Example | Result |
|---|---|---|---|
| `separator=TEXT` | Text between values (default: **Saving → Tag separator**) | `%character:separator=+%` | `ia+miku` |
| `count` | Number of values | `%character:count%` | `2` |
| `sort` | Sort alphabetically | `%general:sort%` | |
| `case=lower\|upper_first\|upper\|caps` | `some tag`, `Some tag`, `Some Tag`, `SOME TAG` | `%character:case=upper%` | `Hatsune Miku` |
| `includenamespace` | Prefix each tag with its type | `%all:includenamespace,unsafe%` | `artist:abc general:solo` |
| `excludenamespace=TYPES` | Types left without prefix by `includenamespace` | `%all:includenamespace,excludenamespace=general,unsafe%` | `artist:abc solo` |
| `ignorenamespace=TYPES` | Leave out tags of these types | `%all:ignorenamespace=general%` | |

### Dates

| Option | Effect |
|---|---|
| `format=FORMAT` | Date format, default `MM-dd-yyyy HH.mm` |
| `timezone=ZONE` | `server` (default), `local`, an IANA name such as `UTC` or `Europe/Prague`, or an offset such as `+01:00` |

The format uses Qt's letters, like Grabber: `d`/`dd` day, `ddd`/`dddd` day name, `M`/`MM` month, `MMM`/`MMMM`
month name, `yy`/`yyyy` year, `h`/`hh` or `H`/`HH` hour, `m`/`mm` minute, `s`/`ss` second, `zzz`
milliseconds, `AP`/`ap` AM/PM. Text in single quotes is copied as is.

```text
%date:format=yyyy-MM-dd%          2016-08-18
%date:format='y'yyyy/MM%          y2016/08
```

## Several values in one token

**Tools → Settings → Tag tokens** sets, for each tag type, what `%artist%`, `%copyright%`, `%character%`… become:

| Setting | Default | Effect |
|---|---|---|
| If empty | `anonymous` (artist), `misc` (copyright), `unknown` (character) … | Text used when the image has no tag of that type |
| If several | Keep all | **Keep all**, **One file per value** (the image is saved once per value), **Keep the first N**, **Keep N, then add text**, **Replace by a text** |
| "Several" means more than | 1 | How many values count as several |
| Text added after N | ` (+ %count%)` | `%count%` is the number of values left out, `%total%` the number of values |
| Replacement text | `multiple artists`, `crossover`, `group` … | Used by **Replace by a text** |
| Order | As returned | **By name** sorts the values |

**Shorten copyrights that start with another one** (on by default) drops `pokemon_bw` when the image is also
tagged `pokemon`.

When **One file per value** gives several paths, **Saving → When a filename gives several paths** chooses between
copies and symbolic links.

Right-click a tag and choose **Ignore** to keep it out of every typed token. It still appears in `%all%`.

## Custom tokens

**Tools → Settings → Custom tokens** defines tokens from tag lists, one per line:

```text
pose: standing sitting lying
```

`%pose%` then contains the image's tags from that list, e.g. `sitting`. A custom token with the name of a
built-in token replaces it.

## Conditionals

Text between `<` and `>` is kept only when every token and tag in it is present.

| Format | Image has | Result |
|---|---|---|
| `<%artist%/>%md5%.%ext%` | an artist | `abc/1bc2….jpg` |
| `<%artist%/>%md5%.%ext%` | no artist | `1bc2….jpg` |
| `<"solo"/>%md5%.%ext%` | the tag `solo` | `solo/1bc2….jpg` |
| `<!"solo"?group/>%md5%.%ext%` | no `solo` tag | `group/1bc2….jpg` |
| `<"tag1" & "tag2"?both/>%md5%.%ext%` | both tags | `both/1bc2….jpg` |
| `<"tag1" \| !%artist%?yes:no>/%md5%.%ext%` | neither | `no/1bc2….jpg` |

- `"tag"` tests a tag, `%token%` tests that a token isn't empty, and `!` inverts the test.
- `<condition?then:else>` and `<condition?then>` choose text explicitly. Conditions combine with `&` (and),
  `|` (or) and parentheses.
- Escape `?`, `!`, `:`, `&` and `|` with `^`. Write a literal `<` or `>` as `<<` or `>>`.

## Conditional filenames

**Tools → Settings → Custom tokens → Conditional filenames** replaces the filename or folder for images that match a
condition. One rule per line:

```text
rating:explicit | %md5%.%ext% | ~/Pictures/Booru Grabber/explicit
"comic" | %copyright%/%id%.%ext% |
```

The condition is a tag (`rating:explicit`, `-comic` for "not comic") or a condition like in
[Conditionals](#conditionals), `|` included. Leave the filename or the folder empty to keep the default. When several
rules match, the last one wins.

## JavaScript

A format that starts with `javascript:` is a script whose result is the path. Every token is a variable; list
tokens are arrays named in the plural (`artists`, `copyrights`, `characters`, `generals`), and `date` is a
`Date`.

```text
javascript:(width >= height ? "landscape" : "portrait") + "/" + copyright + "/" + md5 + "." + ext
```

```text
landscape/sword art online/5231166bd271ce0cd0c11d7e3cb342a5.png
portrait/touhou/0498c78f67d5545bea731d4b1051096c.jpg
```

Scripts run in [QuickJS](https://github.com/quickjs-ng/quickjs) and are stopped after 15 seconds.
