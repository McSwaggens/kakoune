provide-module browse %{

declare-option -hidden str browse_current_dir
declare-option bool browse_respect_gitignore true

define-command browse-directory -params 0..1 -docstring %{
    browse-directory [<directory>]: show directory contents in a buffer
    If no directory is provided, uses the current buffer's directory
} %{ evaluate-commands %sh{
    if [ $# -eq 0 ]; then
        dir="${kak_buffile%/*}"
        [ "$dir" = "$kak_buffile" ] && dir="."
    else
        dir="$1"
    fi

    dir=$(cd "$dir" 2>/dev/null && pwd) || {
        printf "fail 'browse-directory: cannot access %s'\n" "$1"
        exit
    }

    bufname="*browse*"

    # Check if we should filter gitignored files
    use_gitignore=false
    if [ "$kak_opt_browse_respect_gitignore" = "true" ] && \
       git -C "$dir" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        use_gitignore=true
    fi

    # List directory contents: directories first (with trailing /), then files
    listing=$(ls -1Ap "$dir" 2>/dev/null | {
        dirs=""
        files=""
        while IFS= read -r entry; do
            # Skip gitignored entries if enabled
            if [ "$use_gitignore" = "true" ]; then
                base="${entry%/}"
                if git -C "$dir" check-ignore -q "$base" 2>/dev/null; then
                    continue
                fi
            fi
            case "$entry" in
                */) dirs="${dirs}${entry}
" ;;
                *)  files="${files}${entry}
" ;;
            esac
        done
        printf '%s%s' "$dirs" "$files"
    })

    printf "try %%{ delete-buffer '%s' }\n" "$bufname"
    printf "edit -scratch '%s'\n" "$bufname"
    printf "set-option buffer browse_current_dir '%s'\n" "$dir"
    printf "set-option buffer filetype browse\n"
    printf "execute-keys '%%d'\n"
    printf "execute-keys 'i%s<esc>gg'\n" "$listing"
    printf "set-option buffer readonly true\n"
}}

define-command -hidden browse-open %{
    # Select the line content first, then extract it
    execute-keys -save-regs '' x
    evaluate-commands %sh{
        line="${kak_selection}"
        dir="${kak_opt_browse_current_dir}"

        # Handle empty selection
        [ -z "$line" ] && exit

        # Remove trailing newline if any
        line="${line%
}"

        case "$line" in
            ../)
                parent="${dir%/*}"
                [ -z "$parent" ] && parent="/"
                printf "browse-directory '%s'\n" "$parent"
                ;;
            */)
                printf "browse-directory '%s/%s'\n" "$dir" "${line%/}"
                ;;
            *)
                printf "edit -existing '%s/%s'\n" "$dir" "$line"
                ;;
        esac
    }
}

define-command -hidden browse-parent %{ evaluate-commands %sh{
    dir="${kak_opt_browse_current_dir}"
    parent="${dir%/*}"
    [ -z "$parent" ] && parent="/"
    printf "browse-directory '%s'\n" "$parent"
}}

}

hook -group browse-highlight global WinSetOption filetype=browse %{
    add-highlighter window/browse group
    add-highlighter window/browse/ regex '^[^\n]+/$' 0:cyan+b
    hook -once -always window WinSetOption filetype=.* %{ remove-highlighter window/browse }
}

hook global WinSetOption filetype=browse %{
    hook buffer -group browse-hooks NormalKey <ret> browse-open
    hook buffer -group browse-hooks NormalKey <backspace> browse-parent
    hook -once -always window WinSetOption filetype=.* %{ remove-hooks buffer browse-hooks }
}

hook -once global KakBegin .* %{ require-module browse }
